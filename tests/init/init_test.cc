#include "mw/streamer/init/init.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

#ifdef __linux__
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mw/streamer/input/ffmpeg_input.h"

#ifdef MW_STREAMER_STATIC_LIBRARY
#include "Common/MediaSource.h"
#include "Common/Runtime.h"
#include "Util/util.h"
#endif

namespace {

using mw::streamer::FfmpegInput;
using mw::streamer::Init;
using mw::streamer::InitConfig;
using mw::streamer::MwStreamerContext;
using mw::streamer::Shutdown;
using ContextOwner = std::unique_ptr<MwStreamerContext, decltype(&Shutdown)>;

InitConfig TestConfig() {
  InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console_enabled = 0;
  return config;
}

void Close(ContextOwner& context) {
  Shutdown(context.get());
  context.release();
}

#ifdef __linux__
int CheckClosedWrites() {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    return 1;
  }
  close(descriptors[0]);
  const char byte = 'x';
  const auto pipe_result = write(descriptors[1], &byte, sizeof(byte));
  const int pipe_error = errno;
  close(descriptors[1]);
  if (pipe_result != -1 || pipe_error != EPIPE) {
    return 2;
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    return 3;
  }
  close(descriptors[0]);
  const auto socket_result = write(descriptors[1], &byte, sizeof(byte));
  const int socket_error = errno;
  close(descriptors[1]);
  if (socket_result != -1 || socket_error != EPIPE) {
    return 4;
  }
  return 0;
}
#endif

}  // namespace

#ifdef __linux__
TEST_CASE("streamer ignores SIGPIPE across shutdown and reinitialization") {
  const pid_t child = fork();
  if (child == 0) {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    if (sigaction(SIGPIPE, &action, nullptr) != 0 ||
        sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) {
      _exit(1);
    }
    try {
      for (int iteration = 0; iteration < 2; ++iteration) {
        ContextOwner context(Init(TestConfig()), &Shutdown);
        const int initialized_result = CheckClosedWrites();
        if (initialized_result != 0) {
          _exit(10 + iteration * 20 + initialized_result);
        }
        Close(context);
        const int shutdown_result = CheckClosedWrites();
        if (shutdown_result != 0) {
          _exit(20 + iteration * 20 + shutdown_result);
        }
      }
    } catch (...) {
      _exit(100);
    }
    _exit(0);
  }

  REQUIRE(child >= 0);
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited == -1 && errno == EINTR);
  REQUIRE(waited == child);
  INFO("Child signal: " << (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
  REQUIRE(WIFEXITED(status));
  INFO("Child exit code: "
       << WEXITSTATUS(status)
       << "; 1: signal setup, 100: exception; phase offsets "
          "10/20/30/40: initialized/shutdown per iteration; "
          "1/2: pipe creation/write, 3/4: socket creation/write");
  CHECK(WEXITSTATUS(status) == 0);
}
#endif

TEST_CASE("streamer initializes and shuts down with async logging") {
  auto config = TestConfig();
  config.log.async_enabled = 1;
  config.log.async_queue_size = 64;
  ContextOwner context(Init(config), &Shutdown);
  REQUIRE(context.get() != nullptr);
  {
    FfmpegInput input;
    CHECK_NOTHROW(input.Stop());
  }
  MW_LOG_INFO_DEFAULT("Initialization lifecycle completed");
  CHECK_NOTHROW(Close(context));
}

TEST_CASE(
    "failed streamer initialization permits a later valid initialization") {
  auto config = TestConfig();
  config.log.modules = nullptr;
  config.log.modules_size = 1;
  REQUIRE_THROWS_AS(Init(config), std::invalid_argument);
  ContextOwner context(Init(TestConfig()), &Shutdown);
  CHECK_NOTHROW(FfmpegInput());
  Close(context);
}

#ifdef MW_STREAMER_STATIC_LIBRARY
TEST_CASE("streamer delegates its ZLM lifecycle and releases the null source") {
  CHECK_THROWS_AS(mediakit::MediaSource::NullMediaSource(), std::logic_error);
  for (int iteration = 0; iteration < 2; ++iteration) {
    ContextOwner context(Init(TestConfig()), &Shutdown);
    auto& source = mediakit::MediaSource::NullMediaSource();
    std::weak_ptr<mediakit::MediaSource> lifetime = source.shared_from_this();
    CHECK_FALSE(lifetime.expired());
    CHECK(source.readerCount() == 0);
    CHECK_THROWS_AS(mediakit::init(), std::logic_error);
    Close(context);
    CHECK(lifetime.expired());
    CHECK_THROWS_AS(mediakit::MediaSource::NullMediaSource(), std::logic_error);
    CHECK_THROWS_AS(toolkit::getCurrentMillisecond(), std::logic_error);
  }
}

TEST_CASE("ZLM public lifecycle supports shutdown and reinitialization") {
  mw::log::Logging logging(TestConfig().log);
  mediakit::RuntimeConfig config;
  config.event_poller_threads = 1;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  for (int iteration = 0; iteration < 2; ++iteration) {
    std::weak_ptr<mediakit::MediaSource> lifetime;
    {
      const std::unique_ptr<mediakit::Runtime, decltype(&mediakit::shutdown)>
          runtime(mediakit::init(config), &mediakit::shutdown);
      lifetime = mediakit::MediaSource::NullMediaSource().shared_from_this();
      CHECK_FALSE(lifetime.expired());
      CHECK_NOTHROW(mediakit::shutdown(nullptr));
      CHECK_THROWS_AS(mediakit::init(config), std::logic_error);
    }
    CHECK(lifetime.expired());
  }
  CHECK_THROWS_AS(mediakit::MediaSource::NullMediaSource(), std::logic_error);
  CHECK_THROWS_AS(toolkit::getCurrentMillisecond(), std::logic_error);
}
#endif

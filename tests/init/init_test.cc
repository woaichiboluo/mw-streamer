#include "mw/streamer/init/init.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/version.h>
}

#ifdef _WIN32
#include <Windows.h>
#endif

#ifdef __linux__
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef MW_STREAMER_STATIC_LIBRARY
#include "Common/MediaSource.h"
#include "Common/Runtime.h"
#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "Util/util.h"
#endif

namespace {

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
  config.log.console.enabled = false;
  return config;
}

void Close(ContextOwner& context) {
  Shutdown(context.get());
  context.release();
}

struct TemporaryDirectory {
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             fmt::format(
                 "mw-init-log-{}",
                 std::chrono::steady_clock::now().time_since_epoch().count())) {
    if (!std::filesystem::create_directory(path)) {
      throw std::runtime_error("cannot create temporary log directory");
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};

struct RestoreFfmpegLogLevel {
  int level = av_log_get_level();
  ~RestoreFfmpegLogLevel() { av_log_set_level(level); }
};

InitConfig FileLogConfig(const std::filesystem::path& path) {
  auto config = TestConfig();
  config.log.rotating_file.enabled = true;
  config.log.rotating_file.path = path.string();
  return config;
}

std::string ReadLog(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("cannot read initialization test log");
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
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

#ifdef _WIN32
TEST_CASE("streamer configures only an attached console for UTF-8") {
  const UINT input_code_page = GetConsoleCP();
  const UINT output_code_page = GetConsoleOutputCP();
  if (input_code_page == 0) {
    ContextOwner context(Init(TestConfig()), &Shutdown);
    CHECK(GetConsoleCP() == 0);
    CHECK(GetConsoleOutputCP() == 0);
    Close(context);
    return;
  }

  struct RestoreCodePages {
    UINT input;
    UINT output;
    ~RestoreCodePages() {
      SetConsoleCP(input);
      SetConsoleOutputCP(output);
    }
  } restore{input_code_page, output_code_page};

  for (int iteration = 0; iteration < 2; ++iteration) {
    REQUIRE(SetConsoleCP(1252));
    REQUIRE(SetConsoleOutputCP(1252));
    ContextOwner context(Init(TestConfig()), &Shutdown);
    CHECK(GetConsoleCP() == CP_UTF8);
    CHECK(GetConsoleOutputCP() == CP_UTF8);
    Close(context);
    CHECK(GetConsoleCP() == CP_UTF8);
    CHECK(GetConsoleOutputCP() == CP_UTF8);
  }
}
#endif

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
  config.log.async.enabled = true;
  config.log.async.queue_size = 64;
  ContextOwner context(Init(config), &Shutdown);
  REQUIRE(context.get() != nullptr);
  MW_LOG_INFO_DEFAULT("Initialization lifecycle completed");
  CHECK_NOTHROW(Close(context));
}

TEST_CASE(
    "failed streamer initialization permits a later valid initialization") {
  auto config = TestConfig();
  config.log.modules = {{"", mw::log::LogLevel::kInfo}};
  REQUIRE_THROWS_AS(Init(config), std::invalid_argument);
  ContextOwner context(Init(TestConfig()), &Shutdown);
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

TEST_CASE("streamer defaults use two event pollers and one work thread") {
  const InitConfig config;
  CHECK(config.event_poller_threads == 2);
  CHECK(config.work_threads == 1);
  CHECK(config.log.modules.at("ffmpeg") == mw::log::LogLevel::kWarning);
  CHECK(config.log.modules.at("zlm") == mw::log::LogLevel::kWarning);
#ifdef MW_STREAMER_STATIC_LIBRARY
  const mediakit::RuntimeConfig native_config;
  CHECK(native_config.event_poller_threads == 2);
  CHECK(native_config.work_threads == 1);
  auto quiet_config = config;
  quiet_config.enable_cpu_affinity = false;
  quiet_config.log.console.enabled = false;
  ContextOwner context(Init(quiet_config), &Shutdown);
  CHECK(toolkit::EventPollerPool::Instance().getExecutorSize() == 2);
  CHECK(toolkit::WorkThreadPool::Instance().getExecutorSize() == 1);
#endif
}

TEST_CASE("FFmpeg logs default to warning and restore the native level") {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "default.log";
  RestoreFfmpegLogLevel restore;
  av_log_set_level(AV_LOG_DEBUG);
  {
    ContextOwner context(Init(FileLogConfig(path)), &Shutdown);
    CHECK(av_log_get_level() == AV_LOG_WARNING);
    av_log(nullptr, AV_LOG_ERROR, "bridge-default-error\n");
    av_log(nullptr, AV_LOG_WARNING, "bridge-default-warning\r\n");
    av_log(nullptr, AV_LOG_INFO, "bridge-default-hidden-info\n");
  }
  CHECK(av_log_get_level() == AV_LOG_DEBUG);
  const auto output = ReadLog(path);
  CHECK(output.find("[ffmpeg]") != std::string::npos);
  CHECK(output.find("bridge-default-error") != std::string::npos);
  CHECK(output.find("bridge-default-warning") != std::string::npos);
  CHECK(output.find("bridge-default-hidden-info") == std::string::npos);
  CHECK(output.find('\r') == std::string::npos);
  CHECK(output.find("bridge-default-warning\n\n") == std::string::npos);
}

TEST_CASE("FFmpeg severity mapping and module filtering are configurable") {
  TemporaryDirectory temporary;
  struct LevelCase {
    mw::log::LogLevel level;
    int native_level;
  };
  const std::array levels{LevelCase{mw::log::LogLevel::kOff, AV_LOG_QUIET},
                          LevelCase{mw::log::LogLevel::kError, AV_LOG_ERROR},
                          LevelCase{mw::log::LogLevel::kInfo, AV_LOG_INFO},
                          LevelCase{mw::log::LogLevel::kDebug, AV_LOG_DEBUG},
                          LevelCase{mw::log::LogLevel::kTrace, AV_LOG_TRACE}};
  struct MessageCase {
    int level;
    const char* token;
    const char* formatted_level;
  };
  const std::array messages{
      MessageCase{AV_LOG_PANIC, "bridge-panic", "[critical]"},
      MessageCase{AV_LOG_FATAL, "bridge-fatal", "[critical]"},
      MessageCase{AV_LOG_ERROR, "bridge-error", "[error]"},
      MessageCase{AV_LOG_WARNING, "bridge-warning", "[warning]"},
      MessageCase{AV_LOG_INFO, "bridge-info", "[info]"},
      MessageCase{AV_LOG_VERBOSE, "bridge-verbose", "[debug]"},
      MessageCase{AV_LOG_DEBUG, "bridge-debug", "[debug]"},
      MessageCase{AV_LOG_TRACE, "bridge-trace", "[trace]"}};
  for (std::size_t index = 0; index < levels.size(); ++index) {
    const auto path = temporary.path / fmt::format("level-{}.log", index);
    auto config = FileLogConfig(path);
    config.log.modules["ffmpeg"] = levels[index].level;
    {
      ContextOwner context(Init(config), &Shutdown);
      CHECK(av_log_get_level() == levels[index].native_level);
      for (const auto& message : messages) {
        av_log(nullptr, message.level, "%s\n", message.token);
      }
    }
    const auto output = ReadLog(path);
    for (const auto& message : messages) {
      INFO(fmt::format("Configured level index: {}, token: {}", index,
                       message.token));
      const auto position = output.find(message.token);
      if (message.level > levels[index].native_level) {
        CHECK(position == std::string::npos);
      } else {
        REQUIRE(position != std::string::npos);
        const auto line_start = output.rfind('\n', position);
        const auto line = output.substr(
            line_start == std::string::npos ? 0 : line_start + 1,
            position - (line_start == std::string::npos ? 0 : line_start + 1));
        CHECK(line.find(message.formatted_level) != std::string::npos);
      }
    }
  }
}

TEST_CASE(
    "FFmpeg bridge preserves printf arguments context and long messages") {
  TemporaryDirectory temporary;
  const auto path = temporary.path / "messages.log";
  auto config = FileLogConfig(path);
  config.log.modules["ffmpeg"] = mw::log::LogLevel::kTrace;
  const std::string long_message(16384, 'x');
  const AVClass log_class = [] {
    AVClass result{};
    result.class_name = "bridge-context";
    result.item_name = [](void*) -> const char* { return "bridge-context"; };
    result.version = LIBAVUTIL_VERSION_INT;
    return result;
  }();
  struct LogContext {
    const AVClass* av_class;
  } log_context{&log_class};
  {
    ContextOwner context(Init(config), &Shutdown);
    av_log(&log_context, AV_LOG_INFO,
           "bridge-formatted number=%d text=%s percent=%% decimal=%.2f\n", 42,
           "payload", 1.25);
    av_log(nullptr, AV_LOG_TRACE, "bridge-long-begin:%s:bridge-long-end\n",
           long_message.c_str());
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
      threads.emplace_back([thread] {
        for (int message = 0; message < 32; ++message) {
          av_log(nullptr, AV_LOG_DEBUG, "bridge-thread-%d-message-%d|\n",
                 thread, message);
        }
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
  }
  const auto output = ReadLog(path);
  CHECK(output.find("[bridge-context @ ") != std::string::npos);
  CHECK(output.find("number=42 text=payload percent=% decimal=1.25") !=
        std::string::npos);
  CHECK(output.find("bridge-long-begin:" + long_message + ":bridge-long-end") !=
        std::string::npos);
  for (int thread = 0; thread < 4; ++thread) {
    for (int message = 0; message < 32; ++message) {
      const auto token =
          fmt::format("bridge-thread-{}-message-{}|", thread, message);
      const auto position = output.find(token);
      REQUIRE(position != std::string::npos);
      CHECK(output.find(token, position + token.size()) == std::string::npos);
    }
  }
}

TEST_CASE("FFmpeg bridge releases its logger between runtime instances") {
  TemporaryDirectory temporary;
  const auto first_path = temporary.path / "first.log";
  const auto second_path = temporary.path / "second.log";
  {
    ContextOwner context(Init(FileLogConfig(first_path)), &Shutdown);
    av_log(nullptr, AV_LOG_ERROR, "bridge-first-instance\n");
  }
  {
    ContextOwner context(Init(FileLogConfig(second_path)), &Shutdown);
    av_log(nullptr, AV_LOG_ERROR, "bridge-second-instance\n");
  }
  const auto first = ReadLog(first_path);
  const auto second = ReadLog(second_path);
  CHECK(first.find("bridge-first-instance") != std::string::npos);
  CHECK(first.find("bridge-second-instance") == std::string::npos);
  CHECK(second.find("bridge-second-instance") != std::string::npos);
  CHECK(second.find("bridge-first-instance") == std::string::npos);
}

TEST_CASE("failed initialization restores FFmpeg logging before retry") {
  TemporaryDirectory temporary;
  RestoreFfmpegLogLevel restore;
  av_log_set_level(AV_LOG_VERBOSE);
  auto config = FileLogConfig(temporary.path / "failed.log");
  config.log.modules["ffmpeg"] = mw::log::LogLevel::kTrace;
  config.tls.certificate_file = (temporary.path / "missing.pem").string();
  REQUIRE_THROWS(Init(config));
  CHECK(av_log_get_level() == AV_LOG_VERBOSE);
  const auto path = temporary.path / "retry.log";
  {
    ContextOwner context(Init(FileLogConfig(path)), &Shutdown);
    av_log(nullptr, AV_LOG_WARNING, "bridge-retry-instance\n");
  }
  CHECK(av_log_get_level() == AV_LOG_VERBOSE);
  CHECK(ReadLog(path).find("bridge-retry-instance") != std::string::npos);
}

TEST_CASE("invalid performance configuration releases initialization state") {
  auto config = TestConfig();
  SECTION("invalid stage") {
    config.performance.stages[static_cast<mw::streamer::PerformanceStage>(-1)] =
        mw::streamer::PerformanceMode::kSummary;
  }
  SECTION("invalid mode") {
    config.performance.stages[mw::streamer::PerformanceStage::kInput] =
        static_cast<mw::streamer::PerformanceMode>(-1);
  }
  REQUIRE_THROWS_AS(Init(config), std::invalid_argument);
  const auto valid = TestConfig();
  CHECK(valid.performance.stages.empty());
  ContextOwner context(Init(valid), &Shutdown);
  REQUIRE(context != nullptr);
}

#include "mw/streamer/init/init.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

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

}  // namespace

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

#include "mw/streamer/init/init.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>

#include "mw/streamer/input/zlm_input.h"

namespace {

using mw::streamer::Init;
using mw::streamer::InitConfig;
using mw::streamer::MwStreamerContext;
using mw::streamer::Shutdown;
using mw::streamer::ZlmInput;
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
    ZlmInput input;
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
  CHECK_NOTHROW(ZlmInput());
  Close(context);
}

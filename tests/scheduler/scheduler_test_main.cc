#include <fmt/format.h>

#include <catch2/catch_session.hpp>
#include <cstdio>
#include <exception>
#include <memory>

#include "mw/streamer/init/init.h"

int main(int argc, char* argv[]) {
  try {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        context(mw::streamer::Init(config), &mw::streamer::Shutdown);
    return Catch::Session().run(argc, argv);
  } catch (const std::exception& error) {
    fmt::print(stderr, "Test runtime initialization failed: {}\n",
               error.what());
    return 2;
  }
}

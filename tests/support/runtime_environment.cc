#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "mw/log.h"
#include "mw/streamer/init/internal/runtime.h"

namespace {

class RuntimeEnvironment final : public Catch::EventListenerBase {
 public:
  using EventListenerBase::EventListenerBase;

  void testRunStarting(const Catch::TestRunInfo&) override {
    MwLogConfig log_config;
    mw_log_default_config(&log_config);
    MwZlmConfig zlm_config{};
    zlm_config.event_poller_threads = 1;
    zlm_config.work_threads = 1;
    zlm_config.enable_cpu_affinity = 0;
    mw::streamer::internal::InitializeRuntime(&log_config, &zlm_config);
  }

  void testRunEnded(const Catch::TestRunStats&) override {
    mw::streamer::internal::ShutdownRuntime();
  }
};

}  // namespace

CATCH_REGISTER_LISTENER(RuntimeEnvironment)

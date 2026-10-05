#include <catch2/catch_session.hpp>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "mw/streamer/init/init.h"

#ifdef MW_STREAMER_INPUT_TEST_SHARED
#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "Util/util.h"

namespace {

// The HTTP server fixture lives in the executable, whose static SDK copy is
// independent of the SDK owned by mw_streamer.dll.
class FixtureRuntime final {
 public:
  FixtureRuntime() {
    if (toolkit::SockUtil::initialize() != 0) {
      throw std::runtime_error("Cannot initialize fixture networking");
    }
    try {
      clock_ = std::make_unique<toolkit::TimestampClock>();
      toolkit::setTimestampClock(clock_.get());
      toolkit::EventPollerPool::setPoolSize(2);
      toolkit::WorkThreadPool::setPoolSize(1);
      toolkit::EventPollerPool::enableCpuAffinity(false);
      toolkit::WorkThreadPool::enableCpuAffinity(false);
      events_ = toolkit::EventPollerPool::createPool();
      toolkit::EventPollerPool::setInstance(events_.get());
      workers_ = toolkit::WorkThreadPool::createPool();
      toolkit::WorkThreadPool::setInstance(workers_.get());
    } catch (...) {
      Close();
      throw;
    }
  }

  ~FixtureRuntime() { Close(); }

 private:
  void Close() {
    toolkit::WorkThreadPool::releasePool();
    toolkit::WorkThreadPool::setInstance(nullptr);
    workers_.reset();
    toolkit::EventPollerPool::releasePool();
    toolkit::EventPollerPool::setInstance(nullptr);
    events_.reset();
    toolkit::setTimestampClock(nullptr);
    if (clock_) {
      clock_->stop();
      clock_.reset();
    }
    toolkit::SockUtil::release();
  }

  std::unique_ptr<toolkit::TimestampClock> clock_;
  std::unique_ptr<toolkit::EventPollerPool> events_;
  std::unique_ptr<toolkit::WorkThreadPool> workers_;
};

}  // namespace
#endif

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
#ifdef MW_STREAMER_INPUT_TEST_SHARED
    FixtureRuntime fixture_runtime;
#endif
    return Catch::Session().run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Test runtime initialization failed: " << error.what() << '\n';
    return 2;
  }
}

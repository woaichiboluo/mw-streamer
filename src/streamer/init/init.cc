#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "mw/streamer/log/internal/third_party_log_bridge.h"
#include "mw/streamer/init/internal/runtime.h"
#include "srt/SrtEpollReactor.h"

namespace mw::streamer::internal {
namespace {

void ConfigureZlmThreadPools(const ZlmConfig& config) {
  toolkit::EventPollerPool::setPoolSize(config.event_poller_threads);
  toolkit::EventPollerPool::enableCpuAffinity(config.enable_cpu_affinity);
  toolkit::WorkThreadPool::setPoolSize(config.work_threads);
  toolkit::WorkThreadPool::enableCpuAffinity(config.enable_cpu_affinity);
}

class Initializer final {
 public:
  static Initializer& Instance() {
    static auto* initializer = new Initializer;
    return *initializer;
  }

  void EnsureInitialized(const RuntimeConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logging_) {
      return;
    }
    if (IsSrtReactorStopped()) {
      throw std::logic_error(
          "mw-streamer cannot be initialized after the SRT reactor was "
          "stopped");
    }

    std::call_once(init_once_, [this, &config]() {
      logging_.emplace(config.log);
      try {
        ConfigureZlmThreadPools(config.zlm);
        log_bridge_.emplace();
      } catch (...) {
        logging_.reset();
        throw;
      }
      (void)std::atexit(&Initializer::ShutdownAtExit);
    });
    if (!logging_) {
      throw std::logic_error(
          "mw-streamer cannot be initialized after it was shut down");
    }
  }

 private:
  Initializer() = default;

  static void ShutdownAtExit() noexcept {
    auto& initializer = Instance();
    std::lock_guard<std::mutex> lock(initializer.mutex_);
    initializer.log_bridge_.reset();
    initializer.logging_.reset();
  }

  static bool IsSrtReactorStopped() noexcept {
    return mediakit::SrtEpollReactor::isCreated() &&
           !mediakit::SrtEpollReactor::Instance().available();
  }

  std::mutex mutex_;
  std::once_flag init_once_;
  std::optional<mw::log::Logging> logging_;
  std::optional<ThirdPartyLogBridge> log_bridge_;
};

}  // namespace

void EnsureInitialized(const RuntimeConfig& config) {
  Initializer::Instance().EnsureInitialized(config);
}

}  // namespace mw::streamer::internal

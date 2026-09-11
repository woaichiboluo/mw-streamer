#include <mutex>
#include <optional>
#include <stdexcept>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "mw/init/internal/runtime.h"
#include "srt/SrtEpollReactor.h"

namespace mw::streamer::init::internal {
namespace {

void ConfigureZlmThreadPools(const zlm::Config& config) {
  toolkit::EventPollerPool::setPoolSize(config.event_poller_threads);
  toolkit::EventPollerPool::enableCpuAffinity(config.enable_cpu_affinity);
  toolkit::WorkThreadPool::setPoolSize(config.work_threads);
  toolkit::WorkThreadPool::enableCpuAffinity(config.enable_cpu_affinity);
}

class Initializer final {
 public:
  static Initializer& Instance() {
    static Initializer initializer;
    return initializer;
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
      ConfigureZlmThreadPools(config.zlm);
    });
    if (!logging_) {
      throw std::logic_error(
          "mw-streamer cannot be initialized after it was shut down");
    }
  }

 private:
  Initializer() = default;

  static bool IsSrtReactorStopped() noexcept {
    return mediakit::SrtEpollReactor::isCreated() &&
           !mediakit::SrtEpollReactor::Instance().available();
  }

  std::mutex mutex_;
  std::once_flag init_once_;
  std::optional<log::Logging> logging_;
};

}  // namespace

void EnsureInitialized(const RuntimeConfig& config) {
  Initializer::Instance().EnsureInitialized(config);
}

}  // namespace mw::streamer::init::internal

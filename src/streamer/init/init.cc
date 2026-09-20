#include <mutex>
#include <optional>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "mw/streamer/init/internal/runtime.h"
#include "mw/streamer/log/internal/third_party_log_bridge.h"
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
    Initialize(config);
  }

  void Acquire(const RuntimeConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    Initialize(config);
    ++owners_;
  }

  void Release() noexcept {
    // Serialize final cleanup with the next Pipeline's initialization. No
    // Pipeline remains to submit work once the final owner reaches this point.
    std::lock_guard<std::mutex> lock(mutex_);
    if (--owners_ != 0) {
      return;
    }
    mediakit::SrtEpollReactor::destroyIfCreated();
    toolkit::WorkThreadPool::destroyIfCreated();
    toolkit::EventPollerPool::destroyIfCreated();
    log_bridge_.reset();
    logging_.reset();
  }

 private:
  Initializer() = default;

  void Initialize(const RuntimeConfig& config) {
    if (logging_) {
      return;
    }
    logging_.emplace(config.log);
    try {
      ConfigureZlmThreadPools(config.zlm);
      log_bridge_.emplace();
    } catch (...) {
      logging_.reset();
      throw;
    }
  }

  std::mutex mutex_;
  std::size_t owners_ = 0;
  std::optional<mw::log::Logging> logging_;
  std::optional<ThirdPartyLogBridge> log_bridge_;
};

}  // namespace

RuntimeLease::RuntimeLease(const RuntimeConfig& config) {
  Initializer::Instance().Acquire(config);
}

RuntimeLease::~RuntimeLease() { Initializer::Instance().Release(); }

void EnsureInitialized(const RuntimeConfig& config) {
  Initializer::Instance().EnsureInitialized(config);
}

}  // namespace mw::streamer::internal

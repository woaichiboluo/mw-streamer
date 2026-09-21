#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>

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
    EnsureInitializedLocked(config);
  }

  void Acquire(const RuntimeConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitializedLocked(config);
    ++users_;
  }

  void Release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    --users_;
  }

  void Shutdown() {
    if (toolkit::EventPoller::getCurrentPoller()) {
      throw RuntimeStateError("runtime shutdown must run on the host control thread");
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      shutdown_done_.wait(lock, [this] { return state_ != State::kClosing; });
      if (state_ == State::kClosed) return;
      if (state_ == State::kFailed) {
        throw RuntimeStateError("runtime shutdown previously failed");
      }
      if (users_ != 0) {
        throw RuntimeStateError("destroy all Pipelines before runtime shutdown");
      }
      state_ = State::kClosing;
    }
    try {
      ShutdownBackends();
      log_bridge_.reset();
      logging_.reset();
    } catch (...) {
      FinishShutdown(State::kFailed);
      throw;
    }
    FinishShutdown(State::kClosed);
  }

 private:
  enum class State { kOpen, kClosing, kClosed, kFailed };

  void EnsureInitializedLocked(const RuntimeConfig& config) {
    if (state_ != State::kOpen) {
      throw RuntimeStateError("mw-streamer runtime is shut down or shutting down");
    }
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

  void FinishShutdown(State state) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    shutdown_done_.notify_all();
  }

  static void ShutdownBackends() {
    auto* events = toolkit::EventPollerPool::getInstanceIfCreated();
    // The event pool retains both shared and exclusive loops while the
    // producer pool delivers its final completion callbacks.
    mediakit::SrtEpollReactor::shutdownIfCreated();
    if (auto* work = toolkit::WorkThreadPool::getInstanceIfCreated()) {
      work->shutdown();
    }
    if (events) events->shutdown();
    toolkit::shutdownMillisecondThreadIfCreated();
  }

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
  std::condition_variable shutdown_done_;
  State state_ = State::kOpen;
  std::size_t users_ = 0;
  std::once_flag init_once_;
  std::optional<mw::log::Logging> logging_;
  std::optional<ThirdPartyLogBridge> log_bridge_;
};

}  // namespace

void EnsureInitialized(const RuntimeConfig& config) {
  Initializer::Instance().EnsureInitialized(config);
}

RuntimeUse::RuntimeUse(const RuntimeConfig& config) {
  Initializer::Instance().Acquire(config);
}

RuntimeUse::~RuntimeUse() { Initializer::Instance().Release(); }

void ShutdownRuntime() { Initializer::Instance().Shutdown(); }

}  // namespace mw::streamer::internal

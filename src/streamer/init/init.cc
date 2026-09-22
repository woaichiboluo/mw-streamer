#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "mw/log.h"
#include "mw/streamer/init/internal/runtime.h"
#include "mw/streamer/log/internal/third_party_log_bridge.h"
#include "srt/SrtEpollReactor.h"

namespace mw::streamer::internal {
namespace {

void ConfigureZlmThreadPools(const MwZlmConfig& config) {
  toolkit::EventPollerPool::setPoolSize(config.event_poller_threads);
  toolkit::EventPollerPool::enableCpuAffinity(config.enable_cpu_affinity != 0);
  toolkit::WorkThreadPool::setPoolSize(config.work_threads);
  toolkit::WorkThreadPool::enableCpuAffinity(config.enable_cpu_affinity != 0);
}

const char* LogInitializationError(MwLogResult result) {
  switch (result) {
    case kMwLogInvalidArgument:
      return "invalid MwLogConfig";
    case kMwLogAlreadyInitialized:
      return "mw_log is already initialized";
    case kMwLogInternalError:
      return "mw_log initialization failed";
    case kMwLogSuccess:
      break;
  }
  return "unknown mw_log initialization error";
}

class Initializer final {
 public:
  static Initializer& Instance() {
    static auto* initializer = new Initializer;
    return *initializer;
  }

  void Initialize(const MwLogConfig* log_config,
                  const MwZlmConfig* zlm_config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kUninitialized) {
      throw RuntimeStateError(
          "mw-streamer runtime is already initialized or shut down");
    }

    if (!log_config || !zlm_config) {
      throw std::invalid_argument("runtime configurations cannot be null");
    }
    const auto log_result = mw_log_initialize(log_config);
    if (log_result != kMwLogSuccess) {
      throw std::invalid_argument(LogInitializationError(log_result));
    }
    const auto network_result = toolkit::SockUtil::initialize();
    if (network_result != 0) {
      mw_log_shutdown();
      throw std::runtime_error("failed to initialize WinSock: " +
                               std::to_string(network_result));
    }
    try {
      ConfigureZlmThreadPools(*zlm_config);
      log_bridge_.emplace();
      state_ = State::kOpen;
    } catch (...) {
      log_bridge_.reset();
      toolkit::SockUtil::release();
      mw_log_shutdown();
      throw;
    }
  }

  bool IsInitialized() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kOpen;
  }

  void Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireLocked();
    ++users_;
  }

  void Release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    --users_;
  }

  void Shutdown() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      shutdown_done_.wait(lock, [this] { return state_ != State::kClosing; });
      if (state_ == State::kUninitialized) return;
      if (state_ == State::kClosed) return;
      if (state_ == State::kFailed) {
        throw RuntimeStateError("runtime shutdown previously failed");
      }
      if (users_ != 0) {
        throw RuntimeStateError(
            "destroy all Pipelines before runtime shutdown");
      }
      state_ = State::kClosing;
    }
    try {
      mediakit::SrtEpollReactor::release();
      toolkit::WorkThreadPool::releasePool();
      toolkit::EventPollerPool::releasePool();
      const auto network_result = toolkit::SockUtil::release();
      if (network_result != 0) {
        throw std::runtime_error("failed to release WinSock: " +
                                 std::to_string(network_result));
      }
      FinishShutdown(State::kClosed);
    } catch (...) {
      FinishShutdown(State::kFailed);
      throw;
    }
  }

 private:
  enum class State { kUninitialized, kOpen, kClosing, kClosed, kFailed };

  void RequireLocked() const {
    if (state_ != State::kOpen) {
      throw RuntimeStateError("mw-streamer runtime is not initialized");
    }
  }

  void FinishShutdown(State state) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    shutdown_done_.notify_all();
  }

  Initializer() = default;

  std::mutex mutex_;
  std::condition_variable shutdown_done_;
  State state_ = State::kUninitialized;
  std::size_t users_ = 0;
  std::optional<ThirdPartyLogBridge> log_bridge_;
};

}  // namespace

void InitializeRuntime(const MwLogConfig* log_config,
                       const MwZlmConfig* zlm_config) {
  Initializer::Instance().Initialize(log_config, zlm_config);
}

bool IsRuntimeInitialized() noexcept {
  return Initializer::Instance().IsInitialized();
}

RuntimeUse::RuntimeUse() { Initializer::Instance().Acquire(); }

RuntimeUse::~RuntimeUse() { Initializer::Instance().Release(); }

void ShutdownRuntime() { Initializer::Instance().Shutdown(); }

}  // namespace mw::streamer::internal

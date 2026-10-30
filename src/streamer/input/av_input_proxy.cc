#include "mw/streamer/input/av_input_proxy.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"

namespace mw::streamer {
namespace {

const char* StateName(AvInputProxyState state) {
  switch (state) {
    case AvInputProxyState::kStopped:
      return "stopped";
    case AvInputProxyState::kConnecting:
      return "connecting";
    case AvInputProxyState::kConnected:
      return "connected";
    case AvInputProxyState::kWaitingRetry:
      return "waiting_retry";
    case AvInputProxyState::kEnded:
      return "ended";
    case AvInputProxyState::kFailed:
      return "failed";
  }
  return "unknown";
}

bool Terminal(AvInputProxyState state) {
  return state == AvInputProxyState::kStopped ||
         state == AvInputProxyState::kEnded ||
         state == AvInputProxyState::kFailed;
}

std::chrono::seconds RetryDelay(std::uint64_t retry_count) {
  constexpr std::array<int, 4> kDelays{1, 2, 4, 8};
  const auto index =
      static_cast<std::size_t>(std::min(retry_count - 1, std::uint64_t{3}));
  return std::chrono::seconds(kDelays[index]);
}

}  // namespace

class AvInputProxy::Impl final {
 public:
  explicit Impl(AvInputProxyConfig config) : config_(std::move(config)) {
    if (config_.max_retries < -1) {
      throw std::invalid_argument("AvInputProxy 重连次数必须不小于 -1");
    }
    // Validate the same input configuration before starting any worker.
    AvInput validation(config_.input);
  }

  ~Impl() { Stop(); }

  void SetOnStateChanged(OnStateChanged callback) {
    if (worker_.joinable()) {
      throw std::logic_error("配置 AvInputProxy 回调前需要 Stop");
    }
    on_state_changed_ = std::move(callback);
  }

  bool Start(std::string_view url) noexcept {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      MW_LOG_ERROR("streamer", "AvInputProxy[{}] 启动失败: 地址无效",
                   static_cast<const void*>(this));
      return false;
    }
    if (worker_.joinable()) {
      MW_LOG_ERROR("streamer", "AvInputProxy[{}] 启动失败: 重新启动前需要 Stop",
                   static_cast<const void*>(this));
      return false;
    }
    url_ = url;
    MW_LOG_INFO("streamer", "AvInputProxy[{}] 启动: 循环={} 最大重试={}",
                static_cast<const void*>(this), config_.loop,
                config_.max_retries);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      available_ = false;
      read_state_changed_ = false;
      status_ = {};
      status_.state = AvInputProxyState::kConnecting;
      terminal_.reset();
    }
    if (worker_.Start([this]() noexcept { Run(); })) return true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      available_ = false;
      status_ = {};
      terminal_.reset();
    }
    url_.clear();
    changed_.notify_all();
    return false;
  }

  AvInputProxyReadResult ReadPacket(bool wait,
                                    bool return_on_state_change = false) {
    for (;;) {
      std::shared_ptr<AvInput> input;
      std::shared_ptr<const AvInputInfo> description;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this, return_on_state_change] {
          return stopping_ || available_ || Terminal(status_.state) ||
                 (return_on_state_change && read_state_changed_) ||
                 (!config_.loop && terminal_ &&
                  terminal_->state == AvInputState::kEnded);
        };
        if (wait) changed_.wait(lock, ready);
        description = status_.info;
        if (return_on_state_change && read_state_changed_) {
          read_state_changed_ = false;
          return {AvInputReadStatus::kNull, {}, description};
        }
        if (stopping_ || status_.state == AvInputProxyState::kStopped ||
            status_.state == AvInputProxyState::kFailed) {
          return {AvInputReadStatus::kNull, {}, description};
        }
        if (status_.state == AvInputProxyState::kEnded ||
            (!config_.loop && terminal_ &&
             terminal_->state == AvInputState::kEnded)) {
          return {AvInputReadStatus::kEof, {}, description};
        }
        if (!available_) {
          return {AvInputReadStatus::kNull, {}, description};
        }
        input = input_;
      }
      auto result = wait ? input->ReadPacket() : input->TryReadPacket();
      if (result.status == AvInputReadStatus::kPacket) {
        MW_LOG_TRACE("streamer", "AvInputProxy[{}] 返回包: 流={}",
                     static_cast<const void*>(this),
                     (*result.packet)->stream_index);
        return {result.status, std::move(result.packet), description};
      }
      if (result.status == AvInputReadStatus::kEof) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          // Only the consumer acknowledges EOF. Keep this input's result
          // intact even if the control worker immediately replaces the input.
          if (!stopping_ && input_ == input && !terminal_) {
            terminal_ = AvInputStatus{AvInputState::kEnded, {}};
            if (config_.loop) available_ = false;
          }
        }
        changed_.notify_all();
        MW_LOG_DEBUG("streamer", "AvInputProxy[{}] 消费 EOF: 循环={}",
                     static_cast<const void*>(this), config_.loop);
        return {result.status, {}, description};
      }
      if (!wait) return {AvInputReadStatus::kNull, {}, description};
      std::unique_lock<std::mutex> lock(mutex_);
      const auto changed = [this, &input, return_on_state_change] {
        return stopping_ || Terminal(status_.state) ||
               (return_on_state_change && read_state_changed_) ||
               (available_ && input_ != input);
      };
      changed_.wait(lock, changed);
    }
  }

  void RequestStop() noexcept {
    std::shared_ptr<AvInput> input;
    bool requested;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requested = !stopping_;
      stopping_ = true;
      input = input_;
    }
    changed_.notify_all();
    if (input) input->RequestCancel();
    if (requested) {
      MW_LOG_DEBUG("streamer", "AvInputProxy[{}] 请求停止",
                   static_cast<const void*>(this));
    }
  }

  void Stop() noexcept {
    RequestStop();
    worker_.Join();
  }

  AvInputProxyStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  std::shared_ptr<const AvInputInfo> info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_.info;
  }

 private:
  void InputChanged(const std::shared_ptr<AvInput>& input,
                    const AvInputStatus& status) {
    std::shared_ptr<AvInput> cancel;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (input != input_) return;
      // Open resets its cancellation flag before notifying Opening. Reapply a
      // concurrent stop here so stopping before Open cannot lose cancellation.
      if (stopping_ && status.state == AvInputState::kOpening) cancel = input_;
      // Ended is acknowledged by Read, so reconnection cannot hide source EOF.
      if (!terminal_ && (status.state == AvInputState::kFailed ||
                         status.state == AvInputState::kCancelled)) {
        terminal_ = status;
      }
    }
    changed_.notify_all();
    if (cancel) cancel->RequestCancel();
  }

  void Publish(AvInputProxyState state, std::uint64_t retries = 0,
               std::chrono::seconds delay = {}, std::exception_ptr error = {}) {
    AvInputProxyStatus snapshot;
    AvInputProxyState previous;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      previous = status_.state;
      status_.state = state;
      status_.retry_count = retries;
      status_.retry_delay = delay;
      status_.error = std::move(error);
      snapshot = status_;
    }
    changed_.notify_all();
    if (state == AvInputProxyState::kConnected ||
        state == AvInputProxyState::kStopped) {
      MW_LOG_INFO("streamer", "AvInputProxy[{}] 状态: {} -> {}",
                  static_cast<const void*>(this), StateName(previous),
                  StateName(state));
    } else if (state == AvInputProxyState::kWaitingRetry) {
      MW_LOG_WARNING("streamer",
                     "AvInputProxy[{}] 等待重连: 重试={}/{} 延迟={}s",
                     static_cast<const void*>(this), retries,
                     config_.max_retries, delay.count());
    } else if (state == AvInputProxyState::kFailed) {
      MW_LOG_ERROR("streamer", "AvInputProxy[{}] 输入终止: 重试={}/{}",
                   static_cast<const void*>(this), retries,
                   config_.max_retries);
    } else {
      MW_LOG_DEBUG("streamer", "AvInputProxy[{}] 状态: {} -> {} 重试={}",
                   static_cast<const void*>(this), StateName(previous),
                   StateName(state), retries);
    }
    const auto failure_level = state == AvInputProxyState::kWaitingRetry
                                   ? log::LogLevel::kWarning
                                   : log::LogLevel::kError;
    if (snapshot.error && log::ShouldLog("streamer", failure_level)) {
      try {
        std::rethrow_exception(snapshot.error);
      } catch (const ffmpeg::FfmpegException& failure) {
        if (state == AvInputProxyState::kWaitingRetry) {
          MW_LOG_WARNING("streamer", "AvInputProxy[{}] 重连原因: 错误码={} {}",
                         static_cast<const void*>(this), failure.error_code(),
                         failure.what());
        } else {
          MW_LOG_ERROR("streamer", "AvInputProxy[{}] 终止原因: 错误码={} {}",
                       static_cast<const void*>(this), failure.error_code(),
                       failure.what());
        }
      } catch (const std::exception& failure) {
        if (state == AvInputProxyState::kWaitingRetry) {
          MW_LOG_WARNING("streamer", "AvInputProxy[{}] 重连原因: {}",
                         static_cast<const void*>(this), failure.what());
        } else {
          MW_LOG_ERROR("streamer", "AvInputProxy[{}] 终止原因: {}",
                       static_cast<const void*>(this), failure.what());
        }
      } catch (...) {
        if (state == AvInputProxyState::kWaitingRetry) {
          MW_LOG_WARNING("streamer", "AvInputProxy[{}] 重连原因: 未知异常",
                         static_cast<const void*>(this));
        } else {
          MW_LOG_ERROR("streamer", "AvInputProxy[{}] 终止原因: 未知异常",
                       static_cast<const void*>(this));
        }
      }
    }
    if (on_state_changed_) {
      try {
        on_state_changed_(snapshot);
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer", "AvInputProxy[{}] 状态通知异常: {}",
                     static_cast<const void*>(this), failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "AvInputProxy[{}] 状态通知发生未知异常",
                     static_cast<const void*>(this));
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Publish the wakeup after the observer has queued its control event.
      read_state_changed_ = true;
    }
    changed_.notify_all();
  }

  bool Stopping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
  }

  void Retire(const std::shared_ptr<AvInput>& input) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      available_ = false;
    }
    input->Close();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      input_.reset();
    }
    changed_.notify_all();
  }

  void Run() noexcept {
    std::uint64_t retries = 0;
    try {
      while (!Stopping()) {
        auto input = std::make_shared<AvInput>(config_.input);
        input->SetOnStateChanged([this, weak_input = std::weak_ptr<AvInput>(
                                            input)](const auto& status) {
          if (const auto current = weak_input.lock())
            InputChanged(current, status);
        });
        {
          std::lock_guard<std::mutex> lock(mutex_);
          input_ = input;
          status_.info.reset();
          terminal_.reset();
        }
        MW_LOG_DEBUG("streamer", "AvInputProxy[{}] 连接尝试: AvInput={}",
                     static_cast<const void*>(this),
                     static_cast<const void*>(input.get()));
        Publish(AvInputProxyState::kConnecting, retries);
        if (Stopping()) {
          Retire(input);
          break;
        }
        AvInputStatus terminal;
        try {
          input->Open(url_);
          auto description = std::make_shared<const AvInputInfo>(input->info());
          retries = 0;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.info = std::move(description);
          }
          if (!Stopping()) Publish(AvInputProxyState::kConnected);
          {
            std::lock_guard<std::mutex> lock(mutex_);
            // The controller must see Connected before it can block on the
            // new input's packet queue, which may remain empty indefinitely.
            available_ = !stopping_;
          }
          changed_.notify_all();
          {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || terminal_; });
            if (!stopping_) terminal = *terminal_;
          }
        } catch (...) {
          // Open also reports Failed. Handle its original exception once here;
          // the queued notification is cleared when the next attempt begins.
          terminal = {AvInputState::kFailed, std::current_exception()};
        }
        Retire(input);
        if (Stopping()) break;
        if (terminal.state == AvInputState::kEnded) {
          if (!config_.loop) {
            Publish(AvInputProxyState::kEnded);
            return;
          }
          MW_LOG_DEBUG("streamer", "AvInputProxy[{}] 循环重开",
                       static_cast<const void*>(this));
        } else if (terminal.state == AvInputState::kFailed) {
          if (config_.max_retries >= 0 &&
              retries >= static_cast<std::uint64_t>(config_.max_retries)) {
            Publish(AvInputProxyState::kFailed, retries, {}, terminal.error);
            return;
          }
          ++retries;
          const auto delay = RetryDelay(retries);
          Publish(AvInputProxyState::kWaitingRetry, retries, delay,
                  terminal.error);
          std::unique_lock<std::mutex> lock(mutex_);
          if (changed_.wait_for(lock, delay, [this] { return stopping_; }))
            break;
        } else {
          break;
        }
      }
    } catch (...) {
      const auto error = std::current_exception();
      std::shared_ptr<AvInput> input;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        input = input_;
      }
      if (input) Retire(input);
      if (!Stopping()) {
        Publish(AvInputProxyState::kFailed, retries, {}, error);
        return;
      }
    }
    Publish(AvInputProxyState::kStopped);
  }

  const AvInputProxyConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool stopping_ = false;
  bool available_ = false;
  bool read_state_changed_ = false;
  AvInputProxyStatus status_;
  std::optional<AvInputStatus> terminal_;
  std::shared_ptr<AvInput> input_;
  std::string url_;
  Thread worker_;
  OnStateChanged on_state_changed_;
};

AvInputProxy::AvInputProxy(AvInputProxyConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
AvInputProxy::~AvInputProxy() = default;
void AvInputProxy::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
bool AvInputProxy::Start(std::string_view url) noexcept {
  return impl_->Start(url);
}
AvInputProxyReadResult AvInputProxy::ReadPacket() {
  return impl_->ReadPacket(true);
}
AvInputProxyReadResult AvInputProxy::ReadPacketOrStateChange() {
  return impl_->ReadPacket(true, true);
}
AvInputProxyReadResult AvInputProxy::TryReadPacket() {
  return impl_->ReadPacket(false);
}
void AvInputProxy::RequestStop() noexcept { impl_->RequestStop(); }
void AvInputProxy::Stop() noexcept { impl_->Stop(); }
AvInputProxyStatus AvInputProxy::status() const { return impl_->status(); }
std::shared_ptr<const AvInputInfo> AvInputProxy::info() const {
  return impl_->info();
}

}  // namespace mw::streamer

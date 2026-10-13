#include "mw/streamer/input/av_input_proxy.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "mw/log.h"

namespace mw::streamer {
namespace {

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

  void Start(std::string_view url) {
    if (url.empty() || url.find('\0') != std::string_view::npos ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("AvInputProxy 地址不能为空或包含空字符");
    }
    if (worker_.joinable()) {
      throw std::logic_error("重新启动 AvInputProxy 前需要 Stop");
    }
    url_ = url;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = false;
      available_ = false;
      status_ = {};
      status_.state = AvInputProxyState::kConnecting;
      terminal_.reset();
    }
    try {
      worker_ = std::thread(&Impl::Run, this);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = AvInputProxyState::kFailed;
      status_.error = std::current_exception();
      changed_.notify_all();
      throw;
    }
  }

  AvInputProxyReadResult Read(bool wait) {
    for (;;) {
      std::shared_ptr<AvInput> input;
      std::shared_ptr<const AvInputInfo> description;
      std::uint64_t generation = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (wait) {
          changed_.wait(lock, [this] {
            return stopping_ || available_ || Terminal(status_.state);
          });
        }
        generation = status_.generation;
        description = status_.info;
        if (stopping_ || status_.state == AvInputProxyState::kStopped ||
            status_.state == AvInputProxyState::kFailed) {
          return {AvInputReadStatus::kNull, {}, generation, description};
        }
        if (status_.state == AvInputProxyState::kEnded) {
          return {AvInputReadStatus::kEof, {}, generation, description};
        }
        if (!available_) {
          return {AvInputReadStatus::kNull, {}, generation, description};
        }
        input = input_;
      }
      auto result = wait ? input->ReadPacket() : input->TryReadPacket();
      if (result.status == AvInputReadStatus::kPacket) {
        return {result.status, std::move(result.packet), generation,
                description};
      }
      if (result.status == AvInputReadStatus::kEof && !config_.loop) {
        return {result.status, {}, generation, description};
      }
      if (!wait) {
        return {AvInputReadStatus::kNull, {}, generation, description};
      }
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this, &input] {
        return stopping_ || Terminal(status_.state) ||
               (available_ && input_ != input);
      });
    }
  }

  void RequestStop() noexcept {
    std::shared_ptr<AvInput> input;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      input = input_;
    }
    changed_.notify_all();
    if (input) input->RequestCancel();
  }

  void Stop() noexcept {
    RequestStop();
    if (worker_.joinable()) worker_.join();
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
  void InputChanged(std::uint64_t generation, const AvInputStatus& status) {
    std::shared_ptr<AvInput> cancel;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != status_.generation) return;
      // Open resets its cancellation flag before notifying Opening. Reapply a
      // concurrent stop here so stopping before Open cannot lose cancellation.
      if (stopping_ && status.state == AvInputState::kOpening) cancel = input_;
      if (!terminal_ && (status.state == AvInputState::kFailed ||
                         status.state == AvInputState::kEnded ||
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
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = state;
      status_.retry_count = retries;
      status_.retry_delay = delay;
      status_.error = std::move(error);
      snapshot = status_;
    }
    changed_.notify_all();
    if (!on_state_changed_) return;
    try {
      on_state_changed_(snapshot);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer", "AvInputProxy 状态通知异常: {}", failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvInputProxy 状态通知发生未知异常");
    }
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
    std::uint64_t generation = 0;
    std::uint64_t retries = 0;
    try {
      while (!Stopping()) {
        auto input = std::make_shared<AvInput>(config_.input);
        input->SetOnStateChanged([this, generation](const auto& status) {
          InputChanged(generation, status);
        });
        {
          std::lock_guard<std::mutex> lock(mutex_);
          input_ = input;
          status_.generation = generation;
          status_.info.reset();
          terminal_.reset();
        }
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
            available_ = !stopping_;
          }
          if (!Stopping()) Publish(AvInputProxyState::kConnected);
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
        ++generation;
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
  AvInputProxyStatus status_;
  std::optional<AvInputStatus> terminal_;
  std::shared_ptr<AvInput> input_;
  std::string url_;
  std::thread worker_;
  OnStateChanged on_state_changed_;
};

AvInputProxy::AvInputProxy(AvInputProxyConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
AvInputProxy::~AvInputProxy() = default;
void AvInputProxy::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
void AvInputProxy::Start(std::string_view url) { impl_->Start(url); }
AvInputProxyReadResult AvInputProxy::ReadPacket() { return impl_->Read(true); }
AvInputProxyReadResult AvInputProxy::TryReadPacket() {
  return impl_->Read(false);
}
void AvInputProxy::RequestStop() noexcept { impl_->RequestStop(); }
void AvInputProxy::Stop() noexcept { impl_->Stop(); }
AvInputProxyStatus AvInputProxy::status() const { return impl_->status(); }
std::shared_ptr<const AvInputInfo> AvInputProxy::info() const {
  return impl_->info();
}

}  // namespace mw::streamer

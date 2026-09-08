#include "mw/input/zlm_input.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/input/player_proxy.h"
#include "mw/media/stream_event.h"
#include "mw/performance/operation_recorder.h"
#include "mw/sink/packet_sink.h"
#include "mw/zlm/internal/config_validator.h"

namespace mw::streamer::input {
namespace {

InputState MapState(input::PlayerState state) noexcept {
  switch (state) {
    case input::PlayerState::kIdle:
      return InputState::kIdle;
    case input::PlayerState::kConnecting:
      return InputState::kConnecting;
    case input::PlayerState::kReady:
      return InputState::kReady;
    case input::PlayerState::kWaitingRetry:
      return InputState::kWaitingRetry;
    case input::PlayerState::kEnded:
      return InputState::kEnded;
    case input::PlayerState::kFailed:
      return InputState::kFailed;
    case input::PlayerState::kStopped:
      return InputState::kStopped;
  }
  std::terminate();
}

media::StreamEndReason MapEndReason(input::PlayerState state) noexcept {
  switch (state) {
    case input::PlayerState::kEnded:
      return media::StreamEndReason::kEof;
    case input::PlayerState::kWaitingRetry:
      return media::StreamEndReason::kInterrupted;
    case input::PlayerState::kFailed:
      return media::StreamEndReason::kFailed;
    case input::PlayerState::kStopped:
      return media::StreamEndReason::kStopped;
    case input::PlayerState::kIdle:
    case input::PlayerState::kConnecting:
    case input::PlayerState::kReady:
      std::terminate();
  }
  std::terminate();
}

}  // namespace

class ZlmInput::Impl final {
 public:
  explicit Impl(ZlmInputConfig config) : config_(std::move(config)) {}

  ~Impl() {
    Stop();
    if (player_) {
      auto poller = player_->poller();
      player_.reset();
      // PlayerProxy disposes its sinks asynchronously. Keep this bridge's
      // owner alive until that task has released all references to it.
      poller->sync([] {});
    }
  }

  void Start(Observer& observer) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (started_ || stopped_) {
      throw std::logic_error("ZlmInput只能启动一次，停止后不能重新启动");
    }
    if (config_.url.empty()) {
      throw std::invalid_argument("输入URL不能为空");
    }
    zlm::internal::ValidatePlayerConfig(config_.player);
    player_ = std::make_unique<input::PlayerProxy>(
        toolkit::EventPollerPool::Instance().extractPoller(),
        config_.reconnect_policy);
    if (player_->poller()->isCurrentThread()) {
      // No callbacks or bridge exist yet, so asynchronous disposal is safe.
      player_.reset();
      throw std::logic_error("ZlmInput控制操作不能在输入执行上下文中调用");
    }
    player_->AddPacketSink(std::make_unique<Bridge>(*this));
    player_->SetOnState(
        [this](std::uint64_t generation, input::PlayerState state,
               const toolkit::SockException& reason, bool will_retry) {
          state_.store(MapState(state), std::memory_order_relaxed);
          if (observer_) {
            observer_->OnInputStateChanged(
                InputStateChanged{generation, MapState(state),
                                  reason ? reason.what() : "", will_retry});
          }
        });
    player_->poller()->sync([this, &observer] { observer_ = &observer; });
    started_ = true;
    try {
      player_->Start(config_.url, config_.player);
    } catch (...) {
      player_->poller()->sync([this] { observer_ = nullptr; });
      throw;
    }
  }

  void Stop() noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (player_ && player_->poller()->isCurrentThread()) {
      std::terminate();
    }
    if (stopped_) {
      return;
    }
    stopped_ = true;
    if (player_) {
      player_->poller()->sync([this] {
        // Stop runs inline on the owner poller and delivers the final events
        // before releasing the observer. No later callback can borrow it.
        player_->Stop();
        observer_ = nullptr;
      });
    }
    state_.store(InputState::kStopped, std::memory_order_relaxed);
  }

  InputState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }

  performance::NodeSnapshot GetPerformance() const {
    performance::NodeSnapshot snapshot;
    snapshot.name = "ZlmInput";
    snapshot.operations.push_back(performance_.GetSnapshot());
    return snapshot;
  }

 private:
  class Bridge final : public sink::PacketSink {
   public:
    explicit Bridge(Impl& owner) : owner_(owner) {}

    void SetStreams(
        std::uint64_t generation,
        const std::vector<ffmpeg::StreamInfo>& streams) noexcept override {
      if (owner_.observer_) {
        if (generation_ && *generation_ != generation) {
          owner_.observer_->OnTimelineReset(media::TimelineReset{
              generation, media::TimelineResetReason::kReconnect,
              std::nullopt});
        }
        owner_.observer_->OnStreamsReady(
            media::StreamsReady{generation, streams});
      }
      generation_ = generation;
    }

    void Write(std::uint64_t generation,
               const ffmpeg::Packet& packet) noexcept override {
      if (owner_.observer_) {
        // Delivery may synchronously execute downstream business work. Input
        // throughput counts packets here without timing that downstream work.
        if (packet.get()) {
          owner_.performance_.AddOutput(1, packet->size);
        }
        owner_.observer_->OnPacket(
            media::PacketReady{generation, packet.Ref()});
      }
    }

    void EndInput(std::uint64_t generation) noexcept override {
      const auto state = owner_.player_->state();
      owner_.state_.store(MapState(state), std::memory_order_relaxed);
      if (owner_.observer_) {
        owner_.observer_->OnInputEnded(
            media::StreamEnded{generation, MapEndReason(state)});
      }
    }

   private:
    Impl& owner_;
    std::optional<std::uint64_t> generation_;
  };

  ZlmInputConfig config_;
  std::mutex control_mutex_;
  bool started_ = false;
  bool stopped_ = false;
  std::atomic<InputState> state_{InputState::kIdle};
  performance::OperationRecorder performance_{
      performance::PerformanceType::kInput, performance::PerformanceUnit::kNone,
      performance::PerformanceUnit::kPacket};
  std::unique_ptr<input::PlayerProxy> player_;
  // Accessed only on the player's owner poller.
  Observer* observer_ = nullptr;
};

ZlmInput::ZlmInput(ZlmInputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ZlmInput::~ZlmInput() = default;

void ZlmInput::Start(Observer& observer) { impl_->Start(observer); }

void ZlmInput::Stop() noexcept { impl_->Stop(); }

InputState ZlmInput::state() const noexcept { return impl_->state(); }

performance::NodeSnapshot ZlmInput::GetPerformance() const {
  return impl_->GetPerformance();
}

}  // namespace mw::streamer::input

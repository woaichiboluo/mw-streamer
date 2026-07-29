#include "mw/input/player_proxy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "Common/config.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Player/MediaPlayer.h"
#include "Poller/EventPoller.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"

namespace mw::streamer::input {
namespace {

void ValidatePolicy(const ReconnectPolicy& policy) {
  if (policy.max_retries < -1) {
    throw std::invalid_argument("max_retries不能小于-1");
  }
  if (policy.min_delay.count() <= 0) {
    throw std::invalid_argument("min_delay必须大于0");
  }
  if (policy.max_delay < policy.min_delay) {
    throw std::invalid_argument("max_delay不能小于min_delay");
  }
  if (policy.delay_step.count() <= 0) {
    throw std::invalid_argument("delay_step必须大于0");
  }
}

}  // namespace

class PlayerProxy::Impl final
    : public std::enable_shared_from_this<PlayerProxy::Impl> {
 public:
  Impl(std::shared_ptr<toolkit::EventPoller> poller,
       ReconnectPolicy reconnect_policy)
      : poller_(poller ? std::move(poller)
                       : toolkit::EventPollerPool::Instance().getPoller()),
        reconnect_policy_(reconnect_policy) {
    ValidatePolicy(reconnect_policy_);
  }

  void SetOnPacket(OnPacket callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_packet_ = std::move(callback);
        },
        false);
  }

  void SetOnStreamsReady(OnStreamsReady callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_streams_ready_ = std::move(callback);
        },
        false);
  }

  void SetOnState(OnState callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_state_ = std::move(callback);
        },
        false);
  }

  void SetOnTimelineReset(OnTimelineReset callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_timeline_reset_ = std::move(callback);
        },
        false);
  }

  void Start(std::string url, toolkit::mINI options) {
    auto self = shared_from_this();
    poller_->async(
        [self, url = std::move(url), options = std::move(options)]() mutable {
          self->StartOnPoller(std::move(url), std::move(options));
        },
        false);
  }

  void Pause(bool paused, OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, paused, completed = std::move(completed)]() mutable {
          self->PauseOnPoller(paused, std::move(completed));
        },
        false);
  }

  void SeekTo(std::chrono::milliseconds position,
              OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, position, completed = std::move(completed)]() mutable {
          self->SeekToOnPoller(position, std::move(completed));
        },
        false);
  }

  void SetPlaybackRate(float rate, OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, rate, completed = std::move(completed)]() mutable {
          self->SetPlaybackRateOnPoller(rate, std::move(completed));
        },
        false);
  }

  void Stop(OnStopped on_stopped) {
    auto self = shared_from_this();
    poller_->async(
        [self, on_stopped = std::move(on_stopped)]() mutable {
          self->StopOnPoller(std::move(on_stopped));
        },
        false);
  }

  void Dispose() {
    auto self = shared_from_this();
    poller_->async([self]() { self->DisposeOnPoller(); }, false);
  }

  PlayerState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }

  std::uint64_t generation() const noexcept {
    return generation_.load(std::memory_order_relaxed);
  }

  std::uint64_t reconnect_count() const noexcept {
    return reconnect_count_.load(std::memory_order_relaxed);
  }

  const std::shared_ptr<toolkit::EventPoller>& poller() const {
    return poller_;
  }

 private:
  struct Binding {
    mediakit::Track::Ptr track;
    mediakit::FrameWriterInterface* delegate = nullptr;
    converter::ZlmPacketConverter::Ptr packet_converter;
  };

  struct Attempt {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> control_epoch{0};
    std::atomic_bool active{true};
    std::atomic_bool accepting_frames{true};
    std::atomic_bool finite{false};
    bool paused = false;
    std::shared_ptr<mediakit::MediaPlayer> player;
    std::vector<Binding> bindings;
  };

  void CancelRetryOnPoller() {
    if (retry_task_) {
      retry_task_->cancel();
      retry_task_.reset();
    }
  }

  void StartOnPoller(std::string url, toolkit::mINI options) {
    const auto current = state();
    if (current != PlayerState::kIdle && current != PlayerState::kEnded &&
        current != PlayerState::kFailed && current != PlayerState::kStopped) {
      NotifyStateOnPoller(
          generation(), current,
          toolkit::SockException(toolkit::Err_other,
                                 "PlayerProxy已有活动的输入，请先停止当前输入"),
          false);
      return;
    }

    CancelRetryOnPoller();
    TeardownAttemptOnPoller();
    url_ = std::move(url);
    options_ = std::move(options);
    consecutive_failures_ = 0;
    BeginAttemptOnPoller();
  }

  void BeginAttemptOnPoller() {
    CancelRetryOnPoller();

    auto attempt = std::make_shared<Attempt>();
    attempt->generation.store(NextGenerationOnPoller(),
                              std::memory_order_release);
    attempt->player = std::make_shared<mediakit::MediaPlayer>(poller_);
    attempt_ = attempt;

    for (const auto& option : options_) {
      (*attempt->player)[option.first] = option.second;
    }
    // PlayerProxy must preserve the source track set. ZLM enables synthetic
    // silent AAC globally by default, which turns video-only inputs into
    // audio-video inputs.
    (*attempt->player)[mediakit::Protocol::kAddMuteAudio] = false;
    // Codec parameters and config frames must be complete before converters
    // and Track delegates are built.
    (*attempt->player)[mediakit::Client::kWaitTrackReady] = true;

    std::weak_ptr<Impl> weak_self = shared_from_this();
    std::weak_ptr<Attempt> weak_attempt = attempt;
    attempt->player->setOnPlayResult(
        [weak_self, weak_attempt](const toolkit::SockException& ex) {
          auto self = weak_self.lock();
          auto current_attempt = weak_attempt.lock();
          if (!self || !current_attempt) {
            return;
          }
          if (ex) {
            current_attempt->active.store(false, std::memory_order_release);
          }
          self->poller_->async(
              [weak_self, current_attempt, ex]() {
                if (auto locked = weak_self.lock()) {
                  locked->HandlePlayResultOnPoller(current_attempt, ex);
                }
              },
              !ex);
        });
    attempt->player->setOnShutdown(
        [weak_self, weak_attempt](const toolkit::SockException& ex) {
          auto self = weak_self.lock();
          auto current_attempt = weak_attempt.lock();
          if (!self || !current_attempt) {
            return;
          }
          const bool finite_eof =
              current_attempt->finite.load(std::memory_order_acquire) &&
              ex.getErrCode() == toolkit::Err_eof;
          if (!finite_eof) {
            current_attempt->active.store(false, std::memory_order_release);
          }
          self->poller_->async(
              [weak_self, current_attempt, ex]() {
                if (auto locked = weak_self.lock()) {
                  locked->HandleShutdownOnPoller(current_attempt, ex);
                }
              },
              false);
        });

    NotifyStateOnPoller(AttemptGeneration(attempt), PlayerState::kConnecting,
                        {}, false);

    try {
      attempt->player->play(url_);
    } catch (const std::exception& ex) {
      attempt->active.store(false, std::memory_order_release);
      const toolkit::SockException error(toolkit::Err_other, ex.what());
      poller_->async(
          [weak_self, attempt, error]() {
            if (auto self = weak_self.lock()) {
              self->HandleSynchronousFailureOnPoller(attempt, error);
            }
          },
          false);
    }
  }

  bool IsCurrentAttempt(const std::shared_ptr<Attempt>& attempt) const {
    return attempt_ == attempt;
  }

  static std::uint64_t AttemptGeneration(
      const std::shared_ptr<Attempt>& attempt) {
    return attempt ? attempt->generation.load(std::memory_order_acquire) : 0;
  }

  std::uint64_t NextGenerationOnPoller() {
    return generation_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void CompleteControlOnPoller(OnControlCompleted completed,
                               ControlResult result,
                               std::uint64_t result_generation) {
    if (completed) {
      completed(result, result_generation);
    }
  }

  std::shared_ptr<Attempt> ControllableAttemptOnPoller(
      OnControlCompleted& completed) {
    auto attempt = attempt_;
    if (state() != PlayerState::kReady || !attempt || !attempt->player ||
        !attempt->active.load(std::memory_order_acquire)) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kInvalidState, generation());
      return nullptr;
    }
    if (!attempt->finite.load(std::memory_order_acquire)) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kNotSupported,
                              AttemptGeneration(attempt));
      return nullptr;
    }
    return attempt;
  }

  void PauseOnPoller(bool paused, OnControlCompleted completed) {
    auto attempt = ControllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }
    if (attempt->paused == paused) {
      CompleteControlOnPoller(std::move(completed), ControlResult::kAccepted,
                              AttemptGeneration(attempt));
      return;
    }

    const bool was_accepting =
        attempt->accepting_frames.load(std::memory_order_acquire);
    attempt->accepting_frames.store(false, std::memory_order_release);
    attempt->control_epoch.fetch_add(1, std::memory_order_acq_rel);
    try {
      attempt->player->pause(paused);
    } catch (const std::exception&) {
      attempt->accepting_frames.store(was_accepting, std::memory_order_release);
      CompleteControlOnPoller(std::move(completed), ControlResult::kFailed,
                              AttemptGeneration(attempt));
      return;
    }

    attempt->paused = paused;
    if (!paused) {
      attempt->accepting_frames.store(true, std::memory_order_release);
    }
    CompleteControlOnPoller(std::move(completed), ControlResult::kAccepted,
                            AttemptGeneration(attempt));
  }

  void SeekToOnPoller(std::chrono::milliseconds position,
                      OnControlCompleted completed) {
    if (position.count() < 0) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kInvalidArgument, generation());
      return;
    }

    auto attempt = ControllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }

    const auto duration = attempt->player->getDuration();
    if (duration <= 0) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kNotSupported,
                              AttemptGeneration(attempt));
      return;
    }
    const auto duration_ms = static_cast<std::int64_t>(
        std::llround(static_cast<double>(duration) * 1000.0));
    if (position.count() > duration_ms) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kInvalidArgument,
                              AttemptGeneration(attempt));
      return;
    }

    attempt->accepting_frames.store(false, std::memory_order_release);
    attempt->control_epoch.fetch_add(1, std::memory_order_acq_rel);
    try {
      // File frames are produced on a worker poller. pause(true) acquires the
      // MP4Reader lock and forms a barrier against a frame already in flight.
      attempt->player->pause(true);
    } catch (const std::exception&) {
      attempt->accepting_frames.store(!attempt->paused,
                                      std::memory_order_release);
      CompleteControlOnPoller(std::move(completed), ControlResult::kFailed,
                              AttemptGeneration(attempt));
      return;
    }

    ResetBindingsOnPoller(attempt);
    const auto new_generation = NextGenerationOnPoller();
    attempt->generation.store(new_generation, std::memory_order_release);
    if (on_timeline_reset_) {
      on_timeline_reset_(new_generation, TimelineResetReason::kSeek, position);
    }

    attempt->accepting_frames.store(true, std::memory_order_release);
    try {
      const auto progress =
          duration_ms > 0
              ? static_cast<float>(static_cast<double>(position.count()) /
                                   duration_ms)
              : 0.0f;
      attempt->player->seekTo(progress);
    } catch (const std::exception&) {
      attempt->accepting_frames.store(false, std::memory_order_release);
      attempt->paused = true;
      CompleteControlOnPoller(std::move(completed), ControlResult::kFailed,
                              new_generation);
      return;
    }

    attempt->paused = false;
    CompleteControlOnPoller(std::move(completed), ControlResult::kAccepted,
                            new_generation);
  }

  void SetPlaybackRateOnPoller(float rate, OnControlCompleted completed) {
    if (!std::isfinite(rate) || rate < 0.1f || rate > 20.0f) {
      CompleteControlOnPoller(std::move(completed),
                              ControlResult::kInvalidArgument, generation());
      return;
    }

    auto attempt = ControllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }

    try {
      attempt->player->speed(rate);
    } catch (const std::exception&) {
      CompleteControlOnPoller(std::move(completed), ControlResult::kFailed,
                              AttemptGeneration(attempt));
      return;
    }

    if (attempt->paused) {
      attempt->control_epoch.fetch_add(1, std::memory_order_acq_rel);
      attempt->accepting_frames.store(true, std::memory_order_release);
      attempt->paused = false;
    }
    CompleteControlOnPoller(std::move(completed), ControlResult::kAccepted,
                            AttemptGeneration(attempt));
  }

  void HandlePlayResultOnPoller(const std::shared_ptr<Attempt>& attempt,
                                const toolkit::SockException& ex) {
    if (!IsCurrentAttempt(attempt)) {
      return;
    }
    if (ex) {
      HandleAttemptFailureOnPoller(attempt, ex);
      return;
    }
    if (!attempt->active.load(std::memory_order_acquire)) {
      return;
    }

    attempt->finite.store(attempt->player->isFinite(),
                          std::memory_order_release);
    try {
      BindTracksOnPoller(attempt);
    } catch (const std::exception& bind_error) {
      attempt->active.store(false, std::memory_order_release);
      const toolkit::SockException error(toolkit::Err_other, bind_error.what());
      std::weak_ptr<Impl> weak_self = shared_from_this();
      poller_->async(
          [weak_self, attempt, error]() {
            if (auto self = weak_self.lock()) {
              self->HandleBindingFailureOnPoller(attempt, error);
            }
          },
          false);
      return;
    }

    CancelRetryOnPoller();
    consecutive_failures_ = 0;
    NotifyStateOnPoller(AttemptGeneration(attempt), PlayerState::kReady, {},
                        false);
  }

  void HandleSynchronousFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                        const toolkit::SockException& ex) {
    if (!IsCurrentAttempt(attempt)) {
      return;
    }
    HandleAttemptFailureOnPoller(attempt, ex);
  }

  void HandleBindingFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                    const toolkit::SockException& ex) {
    if (!IsCurrentAttempt(attempt)) {
      return;
    }
    TeardownAttemptOnPoller();
    NotifyStateOnPoller(AttemptGeneration(attempt), PlayerState::kFailed, ex,
                        false);
  }

  void HandleShutdownOnPoller(const std::shared_ptr<Attempt>& attempt,
                              const toolkit::SockException& ex) {
    if (!IsCurrentAttempt(attempt)) {
      return;
    }

    if (attempt->finite.load(std::memory_order_acquire) &&
        ex.getErrCode() == toolkit::Err_eof) {
      FlushBindingsOnPoller(attempt);
      TeardownAttemptOnPoller();
      NotifyStateOnPoller(AttemptGeneration(attempt), PlayerState::kEnded, ex,
                          false);
      return;
    }

    HandleAttemptFailureOnPoller(attempt, ex);
  }

  void HandleAttemptFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                    const toolkit::SockException& ex) {
    if (!IsCurrentAttempt(attempt)) {
      return;
    }

    const bool finite = attempt->player && attempt->player->isFinite();
    TeardownAttemptOnPoller();

    if (!finite && CanRetry()) {
      ScheduleRetryOnPoller(AttemptGeneration(attempt), ex);
      return;
    }
    NotifyStateOnPoller(AttemptGeneration(attempt), PlayerState::kFailed, ex,
                        false);
  }

  bool CanRetry() const {
    return reconnect_policy_.max_retries < 0 ||
           consecutive_failures_ < reconnect_policy_.max_retries;
  }

  void ScheduleRetryOnPoller(std::uint64_t failed_generation,
                             const toolkit::SockException& ex) {
    const auto scaled_delay =
        reconnect_policy_.delay_step * consecutive_failures_;
    const auto delay =
        std::max(reconnect_policy_.min_delay,
                 std::min(scaled_delay, reconnect_policy_.max_delay));
    ++consecutive_failures_;
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);

    NotifyStateOnPoller(failed_generation, PlayerState::kWaitingRetry, ex,
                        true);

    std::weak_ptr<Impl> weak_self = shared_from_this();
    retry_task_ = poller_->doDelayTask(
        static_cast<std::uint64_t>(delay.count()), [weak_self]() {
          if (auto self = weak_self.lock()) {
            self->BeginAttemptOnPoller();
          }
          return std::uint64_t{0};
        });
  }

  void BindTracksOnPoller(const std::shared_ptr<Attempt>& attempt) {
    auto tracks = attempt->player->getTracks(true);
    if (tracks.empty()) {
      throw std::runtime_error("ZLM播放成功但没有已就绪Track");
    }
    std::sort(tracks.begin(), tracks.end(),
              [](const auto& left, const auto& right) {
                return left->getIndex() < right->getIndex();
              });

    std::vector<ffmpeg::StreamInfo> streams;
    std::vector<Binding> bindings;
    streams.reserve(tracks.size());
    bindings.reserve(tracks.size());

    std::weak_ptr<Impl> weak_self = shared_from_this();
    std::weak_ptr<Attempt> weak_attempt = attempt;

    int stream_index = 0;
    for (const auto& track : tracks) {
      auto codec_parameters =
          std::make_shared<converter::ZlmCodecParametersConverter>(track);
      auto packet_converter =
          std::make_shared<converter::ZlmPacketConverter>(track, stream_index);

      packet_converter->SetOnPacket(
          [weak_self, weak_attempt](const ffmpeg::Packet& packet) {
            auto self = weak_self.lock();
            auto current_attempt = weak_attempt.lock();
            if (!self || !current_attempt ||
                !self->IsCurrentAttempt(current_attempt) ||
                !current_attempt->accepting_frames.load(
                    std::memory_order_acquire) ||
                self->state() != PlayerState::kReady) {
              return false;
            }
            const auto generation =
                current_attempt->generation.load(std::memory_order_acquire);
            return !self->on_packet_ || self->on_packet_(generation, packet);
          });

      ffmpeg::StreamInfo stream;
      stream.stream_index = stream_index;
      stream.codec_parameters = codec_parameters->codec_parameters();
      stream.time_base = codec_parameters->time_base();
      streams.emplace_back(std::move(stream));

      Binding binding;
      binding.track = track;
      binding.packet_converter = std::move(packet_converter);
      bindings.emplace_back(std::move(binding));
      ++stream_index;
    }

    attempt->bindings = std::move(bindings);
    if (on_streams_ready_) {
      on_streams_ready_(AttemptGeneration(attempt), streams);
    }

    for (std::size_t index = 0; index < attempt->bindings.size(); ++index) {
      auto& binding = attempt->bindings[index];
      binding.delegate = binding.track->addDelegate(
          [weak_self, weak_attempt, stream_index = static_cast<int>(index)](
              const mediakit::Frame::Ptr& frame) {
            auto self = weak_self.lock();
            auto current_attempt = weak_attempt.lock();
            if (!self || !current_attempt) {
              return false;
            }
            const auto control_epoch =
                current_attempt->control_epoch.load(std::memory_order_acquire);
            if (!current_attempt->active.load(std::memory_order_acquire) ||
                !current_attempt->accepting_frames.load(
                    std::memory_order_acquire)) {
              return false;
            }
            auto cached_frame = mediakit::Frame::getCacheAbleFrame(frame);
            self->poller_->async(
                [weak_self, current_attempt, control_epoch, stream_index,
                 frame = std::move(cached_frame)]() {
                  if (auto locked = weak_self.lock()) {
                    locked->InputFrameOnPoller(current_attempt, stream_index,
                                               control_epoch, frame);
                  }
                },
                false);
            return true;
          });
    }
  }

  void InputFrameOnPoller(const std::shared_ptr<Attempt>& attempt,
                          int stream_index, std::uint64_t control_epoch,
                          const mediakit::Frame::Ptr& frame) {
    if (!IsCurrentAttempt(attempt) ||
        !attempt->active.load(std::memory_order_acquire) ||
        !attempt->accepting_frames.load(std::memory_order_acquire) ||
        control_epoch !=
            attempt->control_epoch.load(std::memory_order_acquire) ||
        state() != PlayerState::kReady || stream_index < 0 ||
        static_cast<std::size_t>(stream_index) >= attempt->bindings.size()) {
      return;
    }
    attempt->bindings[stream_index].packet_converter->InputFrame(frame);
  }

  void FlushBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      binding.packet_converter->Flush();
    }
  }

  void ResetBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      binding.packet_converter->Reset();
    }
  }

  void DetachBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      if (binding.track && binding.delegate) {
        binding.track->delDelegate(binding.delegate);
        binding.delegate = nullptr;
      }
    }
    ResetBindingsOnPoller(attempt);
    attempt->bindings.clear();
  }

  void TeardownAttemptOnPoller() {
    auto attempt = std::move(attempt_);
    if (!attempt) {
      return;
    }

    attempt->active.store(false, std::memory_order_release);
    DetachBindingsOnPoller(attempt);

    if (attempt->player) {
      attempt->player->setOnPlayResult(nullptr);
      attempt->player->setOnShutdown(nullptr);
      attempt->player->setOnResume(nullptr);
      attempt->player->teardown();
      attempt->player.reset();
    }
  }

  void StopOnPoller(OnStopped on_stopped) {
    CancelRetryOnPoller();
    TeardownAttemptOnPoller();
    consecutive_failures_ = 0;
    NotifyStateOnPoller(
        generation(), PlayerState::kStopped,
        toolkit::SockException(toolkit::Err_shutdown, "PlayerProxy主动停止"),
        false);
    if (on_stopped) {
      on_stopped();
    }
  }

  void DisposeOnPoller() {
    CancelRetryOnPoller();
    on_packet_ = nullptr;
    on_streams_ready_ = nullptr;
    on_state_ = nullptr;
    on_timeline_reset_ = nullptr;
    TeardownAttemptOnPoller();
    consecutive_failures_ = 0;
    state_.store(PlayerState::kStopped, std::memory_order_relaxed);
  }

  void NotifyStateOnPoller(std::uint64_t event_generation,
                           PlayerState new_state,
                           const toolkit::SockException& reason,
                           bool will_retry) {
    state_.store(new_state, std::memory_order_relaxed);
    if (on_state_) {
      on_state_(event_generation, new_state, reason, will_retry);
    }
  }

  std::shared_ptr<toolkit::EventPoller> poller_;
  ReconnectPolicy reconnect_policy_;
  toolkit::mINI options_;
  std::string url_;
  std::shared_ptr<Attempt> attempt_;
  toolkit::TaskCancelableImp<std::uint64_t()>::Ptr retry_task_;

  OnPacket on_packet_;
  OnStreamsReady on_streams_ready_;
  OnState on_state_;
  OnTimelineReset on_timeline_reset_;

  std::atomic<PlayerState> state_{PlayerState::kIdle};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> reconnect_count_{0};
  int consecutive_failures_ = 0;
};

PlayerProxy::PlayerProxy(std::shared_ptr<toolkit::EventPoller> poller,
                         ReconnectPolicy reconnect_policy)
    : impl_(std::make_shared<Impl>(std::move(poller), reconnect_policy)) {}

PlayerProxy::~PlayerProxy() {
  if (impl_) {
    impl_->Dispose();
  }
}

void PlayerProxy::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}

void PlayerProxy::SetOnStreamsReady(OnStreamsReady callback) {
  impl_->SetOnStreamsReady(std::move(callback));
}

void PlayerProxy::SetOnState(OnState callback) {
  impl_->SetOnState(std::move(callback));
}

void PlayerProxy::SetOnTimelineReset(OnTimelineReset callback) {
  impl_->SetOnTimelineReset(std::move(callback));
}

void PlayerProxy::Start(std::string url, toolkit::mINI options) {
  impl_->Start(std::move(url), std::move(options));
}

void PlayerProxy::Pause(bool paused, OnControlCompleted completed) {
  impl_->Pause(paused, std::move(completed));
}

void PlayerProxy::SeekTo(std::chrono::milliseconds position,
                         OnControlCompleted completed) {
  impl_->SeekTo(position, std::move(completed));
}

void PlayerProxy::SetPlaybackRate(float rate, OnControlCompleted completed) {
  impl_->SetPlaybackRate(rate, std::move(completed));
}

void PlayerProxy::Stop(OnStopped on_stopped) {
  impl_->Stop(std::move(on_stopped));
}

PlayerState PlayerProxy::state() const noexcept { return impl_->state(); }

std::uint64_t PlayerProxy::generation() const noexcept {
  return impl_->generation();
}

std::uint64_t PlayerProxy::reconnect_count() const noexcept {
  return impl_->reconnect_count();
}

std::shared_ptr<toolkit::EventPoller> PlayerProxy::poller() const {
  return impl_->poller();
}

}  // namespace mw::streamer::input

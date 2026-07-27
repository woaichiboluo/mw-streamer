#include "mw/input/PlayerProxy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "Common/config.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Player/MediaPlayer.h"
#include "Poller/EventPoller.h"
#include "mw/convter/ZlmCodecParametersConvter.h"
#include "mw/convter/ZlmPacketConvter.h"

namespace mw::input {
namespace {

void validatePolicy(const ReconnectPolicy& policy) {
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
    validatePolicy(reconnect_policy_);
  }

  void setOnPacket(OnPacket callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_packet_ = std::move(callback);
        },
        false);
  }

  void setOnStreamsReady(OnStreamsReady callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_streams_ready_ = std::move(callback);
        },
        false);
  }

  void setOnState(OnState callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_state_ = std::move(callback);
        },
        false);
  }

  void setOnTimelineReset(OnTimelineReset callback) {
    auto self = shared_from_this();
    poller_->async(
        [self, callback = std::move(callback)]() mutable {
          self->on_timeline_reset_ = std::move(callback);
        },
        false);
  }

  void start(std::string url, toolkit::mINI options) {
    auto self = shared_from_this();
    poller_->async(
        [self, url = std::move(url), options = std::move(options)]() mutable {
          self->startOnPoller(std::move(url), std::move(options));
        },
        false);
  }

  void pause(bool paused, OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, paused, completed = std::move(completed)]() mutable {
          self->pauseOnPoller(paused, std::move(completed));
        },
        false);
  }

  void seekTo(std::chrono::milliseconds position,
              OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, position, completed = std::move(completed)]() mutable {
          self->seekToOnPoller(position, std::move(completed));
        },
        false);
  }

  void setPlaybackRate(float rate, OnControlCompleted completed) {
    auto self = shared_from_this();
    poller_->async(
        [self, rate, completed = std::move(completed)]() mutable {
          self->setPlaybackRateOnPoller(rate, std::move(completed));
        },
        false);
  }

  void stop(OnStopped on_stopped) {
    auto self = shared_from_this();
    poller_->async(
        [self, on_stopped = std::move(on_stopped)]() mutable {
          self->stopOnPoller(std::move(on_stopped));
        },
        false);
  }

  void dispose() {
    auto self = shared_from_this();
    poller_->async([self]() { self->disposeOnPoller(); }, false);
  }

  PlayerState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }

  std::uint64_t generation() const noexcept {
    return generation_.load(std::memory_order_relaxed);
  }

  std::uint64_t reconnectCount() const noexcept {
    return reconnect_count_.load(std::memory_order_relaxed);
  }

  const std::shared_ptr<toolkit::EventPoller>& getPoller() const {
    return poller_;
  }

 private:
  struct Binding {
    mediakit::Track::Ptr track;
    mediakit::FrameWriterInterface* delegate = nullptr;
    convter::ZlmPacketConvter::Ptr packet_converter;
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

  void cancelRetryOnPoller() {
    if (retry_task_) {
      retry_task_->cancel();
      retry_task_.reset();
    }
  }

  void startOnPoller(std::string url, toolkit::mINI options) {
    const auto current = state();
    if (current != PlayerState::Idle && current != PlayerState::Ended &&
        current != PlayerState::Failed && current != PlayerState::Stopped) {
      notifyStateOnPoller(
          generation(), current,
          toolkit::SockException(toolkit::Err_other,
                                 "PlayerProxy已有活动的输入，请先停止当前输入"),
          false);
      return;
    }

    cancelRetryOnPoller();
    teardownAttemptOnPoller();
    url_ = std::move(url);
    options_ = std::move(options);
    consecutive_failures_ = 0;
    beginAttemptOnPoller();
  }

  void beginAttemptOnPoller() {
    cancelRetryOnPoller();

    auto attempt = std::make_shared<Attempt>();
    attempt->generation.store(nextGenerationOnPoller(),
                              std::memory_order_release);
    attempt->player = std::make_shared<mediakit::MediaPlayer>(poller_);
    attempt_ = attempt;

    for (const auto& option : options_) {
      (*attempt->player)[option.first] = option.second;
    }
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
                  locked->handlePlayResultOnPoller(current_attempt, ex);
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
                  locked->handleShutdownOnPoller(current_attempt, ex);
                }
              },
              false);
        });

    notifyStateOnPoller(attemptGeneration(attempt), PlayerState::Connecting, {},
                        false);

    try {
      attempt->player->play(url_);
    } catch (const std::exception& ex) {
      attempt->active.store(false, std::memory_order_release);
      const toolkit::SockException error(toolkit::Err_other, ex.what());
      poller_->async(
          [weak_self, attempt, error]() {
            if (auto self = weak_self.lock()) {
              self->handleSynchronousFailureOnPoller(attempt, error);
            }
          },
          false);
    }
  }

  bool isCurrentAttempt(const std::shared_ptr<Attempt>& attempt) const {
    return attempt_ == attempt;
  }

  static std::uint64_t attemptGeneration(
      const std::shared_ptr<Attempt>& attempt) {
    return attempt ? attempt->generation.load(std::memory_order_acquire) : 0;
  }

  std::uint64_t nextGenerationOnPoller() {
    return generation_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  void completeControlOnPoller(OnControlCompleted completed,
                               ControlResult result,
                               std::uint64_t result_generation) {
    if (completed) {
      completed(result, result_generation);
    }
  }

  std::shared_ptr<Attempt> controllableAttemptOnPoller(
      OnControlCompleted& completed) {
    auto attempt = attempt_;
    if (state() != PlayerState::Ready || !attempt || !attempt->player ||
        !attempt->active.load(std::memory_order_acquire)) {
      completeControlOnPoller(std::move(completed), ControlResult::InvalidState,
                              generation());
      return nullptr;
    }
    if (!attempt->finite.load(std::memory_order_acquire)) {
      completeControlOnPoller(std::move(completed), ControlResult::NotSupported,
                              attemptGeneration(attempt));
      return nullptr;
    }
    return attempt;
  }

  void pauseOnPoller(bool paused, OnControlCompleted completed) {
    auto attempt = controllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }
    if (attempt->paused == paused) {
      completeControlOnPoller(std::move(completed), ControlResult::Accepted,
                              attemptGeneration(attempt));
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
      completeControlOnPoller(std::move(completed), ControlResult::Failed,
                              attemptGeneration(attempt));
      return;
    }

    attempt->paused = paused;
    if (!paused) {
      attempt->accepting_frames.store(true, std::memory_order_release);
    }
    completeControlOnPoller(std::move(completed), ControlResult::Accepted,
                            attemptGeneration(attempt));
  }

  void seekToOnPoller(std::chrono::milliseconds position,
                      OnControlCompleted completed) {
    if (position.count() < 0) {
      completeControlOnPoller(std::move(completed),
                              ControlResult::InvalidArgument, generation());
      return;
    }

    auto attempt = controllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }

    const auto duration = attempt->player->getDuration();
    if (duration <= 0) {
      completeControlOnPoller(std::move(completed), ControlResult::NotSupported,
                              attemptGeneration(attempt));
      return;
    }
    const auto duration_ms = static_cast<std::int64_t>(
        std::llround(static_cast<double>(duration) * 1000.0));
    if (position.count() > duration_ms) {
      completeControlOnPoller(std::move(completed),
                              ControlResult::InvalidArgument,
                              attemptGeneration(attempt));
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
      completeControlOnPoller(std::move(completed), ControlResult::Failed,
                              attemptGeneration(attempt));
      return;
    }

    resetBindingsOnPoller(attempt);
    const auto new_generation = nextGenerationOnPoller();
    attempt->generation.store(new_generation, std::memory_order_release);
    if (on_timeline_reset_) {
      on_timeline_reset_(new_generation, TimelineResetReason::Seek, position);
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
      completeControlOnPoller(std::move(completed), ControlResult::Failed,
                              new_generation);
      return;
    }

    attempt->paused = false;
    completeControlOnPoller(std::move(completed), ControlResult::Accepted,
                            new_generation);
  }

  void setPlaybackRateOnPoller(float rate, OnControlCompleted completed) {
    if (!std::isfinite(rate) || rate < 0.1f || rate > 20.0f) {
      completeControlOnPoller(std::move(completed),
                              ControlResult::InvalidArgument, generation());
      return;
    }

    auto attempt = controllableAttemptOnPoller(completed);
    if (!attempt) {
      return;
    }

    try {
      attempt->player->speed(rate);
    } catch (const std::exception&) {
      completeControlOnPoller(std::move(completed), ControlResult::Failed,
                              attemptGeneration(attempt));
      return;
    }

    if (attempt->paused) {
      attempt->control_epoch.fetch_add(1, std::memory_order_acq_rel);
      attempt->accepting_frames.store(true, std::memory_order_release);
      attempt->paused = false;
    }
    completeControlOnPoller(std::move(completed), ControlResult::Accepted,
                            attemptGeneration(attempt));
  }

  void handlePlayResultOnPoller(const std::shared_ptr<Attempt>& attempt,
                                const toolkit::SockException& ex) {
    if (!isCurrentAttempt(attempt)) {
      return;
    }
    if (ex) {
      handleAttemptFailureOnPoller(attempt, ex);
      return;
    }
    if (!attempt->active.load(std::memory_order_acquire)) {
      return;
    }

    attempt->finite.store(attempt->player->isFinite(),
                          std::memory_order_release);
    try {
      bindTracksOnPoller(attempt);
    } catch (const std::exception& bind_error) {
      attempt->active.store(false, std::memory_order_release);
      const toolkit::SockException error(toolkit::Err_other, bind_error.what());
      std::weak_ptr<Impl> weak_self = shared_from_this();
      poller_->async(
          [weak_self, attempt, error]() {
            if (auto self = weak_self.lock()) {
              self->handleBindingFailureOnPoller(attempt, error);
            }
          },
          false);
      return;
    }

    cancelRetryOnPoller();
    consecutive_failures_ = 0;
    notifyStateOnPoller(attemptGeneration(attempt), PlayerState::Ready, {},
                        false);
  }

  void handleSynchronousFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                        const toolkit::SockException& ex) {
    if (!isCurrentAttempt(attempt)) {
      return;
    }
    handleAttemptFailureOnPoller(attempt, ex);
  }

  void handleBindingFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                    const toolkit::SockException& ex) {
    if (!isCurrentAttempt(attempt)) {
      return;
    }
    teardownAttemptOnPoller();
    notifyStateOnPoller(attemptGeneration(attempt), PlayerState::Failed, ex,
                        false);
  }

  void handleShutdownOnPoller(const std::shared_ptr<Attempt>& attempt,
                              const toolkit::SockException& ex) {
    if (!isCurrentAttempt(attempt)) {
      return;
    }

    if (attempt->finite.load(std::memory_order_acquire) &&
        ex.getErrCode() == toolkit::Err_eof) {
      flushBindingsOnPoller(attempt);
      teardownAttemptOnPoller();
      notifyStateOnPoller(attemptGeneration(attempt), PlayerState::Ended, ex,
                          false);
      return;
    }

    handleAttemptFailureOnPoller(attempt, ex);
  }

  void handleAttemptFailureOnPoller(const std::shared_ptr<Attempt>& attempt,
                                    const toolkit::SockException& ex) {
    if (!isCurrentAttempt(attempt)) {
      return;
    }

    const bool finite = attempt->player && attempt->player->isFinite();
    teardownAttemptOnPoller();

    if (!finite && canRetry()) {
      scheduleRetryOnPoller(attemptGeneration(attempt), ex);
      return;
    }
    notifyStateOnPoller(attemptGeneration(attempt), PlayerState::Failed, ex,
                        false);
  }

  bool canRetry() const {
    return reconnect_policy_.max_retries < 0 ||
           consecutive_failures_ < reconnect_policy_.max_retries;
  }

  void scheduleRetryOnPoller(std::uint64_t failed_generation,
                             const toolkit::SockException& ex) {
    const auto scaled_delay =
        reconnect_policy_.delay_step * consecutive_failures_;
    const auto delay =
        std::max(reconnect_policy_.min_delay,
                 std::min(scaled_delay, reconnect_policy_.max_delay));
    ++consecutive_failures_;
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);

    notifyStateOnPoller(failed_generation, PlayerState::WaitingRetry, ex, true);

    std::weak_ptr<Impl> weak_self = shared_from_this();
    retry_task_ = poller_->doDelayTask(
        static_cast<std::uint64_t>(delay.count()), [weak_self]() {
          if (auto self = weak_self.lock()) {
            self->beginAttemptOnPoller();
          }
          return std::uint64_t{0};
        });
  }

  void bindTracksOnPoller(const std::shared_ptr<Attempt>& attempt) {
    auto tracks = attempt->player->getTracks(true);
    if (tracks.empty()) {
      throw std::runtime_error("ZLM播放成功但没有已就绪Track");
    }
    std::sort(tracks.begin(), tracks.end(),
              [](const auto& left, const auto& right) {
                return left->getIndex() < right->getIndex();
              });

    std::vector<StreamInfo> streams;
    std::vector<Binding> bindings;
    streams.reserve(tracks.size());
    bindings.reserve(tracks.size());

    std::weak_ptr<Impl> weak_self = shared_from_this();
    std::weak_ptr<Attempt> weak_attempt = attempt;

    int stream_index = 0;
    for (const auto& track : tracks) {
      auto codec_parameters =
          std::make_shared<convter::ZlmCodecParametersConvter>(track);
      auto packet_converter =
          std::make_shared<convter::ZlmPacketConvter>(track, stream_index);

      packet_converter->setOnPacket(
          [weak_self, weak_attempt](const AVPacket* packet) {
            auto self = weak_self.lock();
            auto current_attempt = weak_attempt.lock();
            if (!self || !current_attempt ||
                !self->isCurrentAttempt(current_attempt) ||
                !current_attempt->accepting_frames.load(
                    std::memory_order_acquire) ||
                self->state() != PlayerState::Ready) {
              return false;
            }
            const auto generation =
                current_attempt->generation.load(std::memory_order_acquire);
            return !self->on_packet_ || self->on_packet_(generation, packet);
          });

      StreamInfo stream;
      stream.stream_index = stream_index;
      stream.codec_parameters = codec_parameters->getCodecParameters();
      stream.time_base = codec_parameters->getTimeBase();
      streams.emplace_back(std::move(stream));

      Binding binding;
      binding.track = track;
      binding.packet_converter = std::move(packet_converter);
      bindings.emplace_back(std::move(binding));
      ++stream_index;
    }

    attempt->bindings = std::move(bindings);
    if (on_streams_ready_) {
      on_streams_ready_(attemptGeneration(attempt), streams);
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
            self->poller_->async(
                [weak_self, current_attempt, control_epoch, stream_index,
                 frame]() {
                  if (auto locked = weak_self.lock()) {
                    locked->inputFrameOnPoller(current_attempt, stream_index,
                                               control_epoch, frame);
                  }
                },
                false);
            return true;
          });
    }
  }

  void inputFrameOnPoller(const std::shared_ptr<Attempt>& attempt,
                          int stream_index, std::uint64_t control_epoch,
                          const mediakit::Frame::Ptr& frame) {
    if (!isCurrentAttempt(attempt) ||
        !attempt->active.load(std::memory_order_acquire) ||
        !attempt->accepting_frames.load(std::memory_order_acquire) ||
        control_epoch !=
            attempt->control_epoch.load(std::memory_order_acquire) ||
        state() != PlayerState::Ready || stream_index < 0 ||
        static_cast<std::size_t>(stream_index) >= attempt->bindings.size()) {
      return;
    }
    attempt->bindings[stream_index].packet_converter->inputFrame(frame);
  }

  void flushBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      binding.packet_converter->flush();
    }
  }

  void resetBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      binding.packet_converter->reset();
    }
  }

  void detachBindingsOnPoller(const std::shared_ptr<Attempt>& attempt) {
    for (auto& binding : attempt->bindings) {
      if (binding.track && binding.delegate) {
        binding.track->delDelegate(binding.delegate);
        binding.delegate = nullptr;
      }
    }
    resetBindingsOnPoller(attempt);
    attempt->bindings.clear();
  }

  void teardownAttemptOnPoller() {
    auto attempt = std::move(attempt_);
    if (!attempt) {
      return;
    }

    attempt->active.store(false, std::memory_order_release);
    detachBindingsOnPoller(attempt);

    if (attempt->player) {
      attempt->player->setOnPlayResult(nullptr);
      attempt->player->setOnShutdown(nullptr);
      attempt->player->setOnResume(nullptr);
      attempt->player->teardown();
      attempt->player.reset();
    }
  }

  void stopOnPoller(OnStopped on_stopped) {
    cancelRetryOnPoller();
    teardownAttemptOnPoller();
    consecutive_failures_ = 0;
    notifyStateOnPoller(
        generation(), PlayerState::Stopped,
        toolkit::SockException(toolkit::Err_shutdown, "PlayerProxy主动停止"),
        false);
    if (on_stopped) {
      on_stopped();
    }
  }

  void disposeOnPoller() {
    cancelRetryOnPoller();
    on_packet_ = nullptr;
    on_streams_ready_ = nullptr;
    on_state_ = nullptr;
    on_timeline_reset_ = nullptr;
    teardownAttemptOnPoller();
    consecutive_failures_ = 0;
    state_.store(PlayerState::Stopped, std::memory_order_relaxed);
  }

  void notifyStateOnPoller(std::uint64_t event_generation,
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

  std::atomic<PlayerState> state_{PlayerState::Idle};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> reconnect_count_{0};
  int consecutive_failures_ = 0;
};

PlayerProxy::PlayerProxy(std::shared_ptr<toolkit::EventPoller> poller,
                         ReconnectPolicy reconnect_policy)
    : impl_(std::make_shared<Impl>(std::move(poller), reconnect_policy)) {}

PlayerProxy::~PlayerProxy() {
  if (impl_) {
    impl_->dispose();
  }
}

void PlayerProxy::setOnPacket(OnPacket callback) {
  impl_->setOnPacket(std::move(callback));
}

void PlayerProxy::setOnStreamsReady(OnStreamsReady callback) {
  impl_->setOnStreamsReady(std::move(callback));
}

void PlayerProxy::setOnState(OnState callback) {
  impl_->setOnState(std::move(callback));
}

void PlayerProxy::setOnTimelineReset(OnTimelineReset callback) {
  impl_->setOnTimelineReset(std::move(callback));
}

void PlayerProxy::start(std::string url, toolkit::mINI options) {
  impl_->start(std::move(url), std::move(options));
}

void PlayerProxy::pause(bool paused, OnControlCompleted completed) {
  impl_->pause(paused, std::move(completed));
}

void PlayerProxy::seekTo(std::chrono::milliseconds position,
                         OnControlCompleted completed) {
  impl_->seekTo(position, std::move(completed));
}

void PlayerProxy::setPlaybackRate(float rate, OnControlCompleted completed) {
  impl_->setPlaybackRate(rate, std::move(completed));
}

void PlayerProxy::stop(OnStopped on_stopped) {
  impl_->stop(std::move(on_stopped));
}

PlayerState PlayerProxy::state() const noexcept { return impl_->state(); }

std::uint64_t PlayerProxy::generation() const noexcept {
  return impl_->generation();
}

std::uint64_t PlayerProxy::reconnectCount() const noexcept {
  return impl_->reconnectCount();
}

std::shared_ptr<toolkit::EventPoller> PlayerProxy::getPoller() const {
  return impl_->getPoller();
}

}  // namespace mw::input

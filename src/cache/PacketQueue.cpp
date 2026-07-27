#include "mw/cache/PacketQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "Poller/EventPoller.h"

namespace mw::cache {
namespace {

using Clock = std::chrono::steady_clock;
using PacketPtr = std::shared_ptr<AVPacket>;

constexpr AVRational kComparisonTimeBase{1, AV_TIME_BASE};

PacketPtr clonePacket(const AVPacket* packet) {
  return PacketPtr(av_packet_clone(packet),
                   [](AVPacket* owned) { av_packet_free(&owned); });
}

void validateCacheDuration(std::chrono::milliseconds duration) {
  using namespace std::chrono_literals;
  if (duration < 1s || duration > 30s) {
    throw std::invalid_argument("cache_duration必须在1秒到30秒之间");
  }
}

void validateStreams(const std::vector<PacketStream>& streams) {
  std::size_t audio_count = 0;
  std::size_t video_count = 0;

  for (const auto& stream : streams) {
    if (stream.stream_index < 0 || stream.time_base.num <= 0 ||
        stream.time_base.den <= 0) {
      throw std::invalid_argument("PacketStream参数无效");
    }
    if (stream.media_type == AVMEDIA_TYPE_AUDIO) {
      ++audio_count;
    } else if (stream.media_type == AVMEDIA_TYPE_VIDEO) {
      ++video_count;
    } else {
      throw std::invalid_argument("PacketQueue只支持音频和视频轨道");
    }
  }

  if (audio_count != 1 || video_count != 1 || streams.size() != 2) {
    throw std::invalid_argument("PacketQueue需要且仅支持一路音频和一路视频");
  }
  if (streams[0].stream_index == streams[1].stream_index) {
    throw std::invalid_argument("音频和视频stream_index不能相同");
  }
}

}  // namespace

class PacketQueue::Impl final
    : public std::enable_shared_from_this<PacketQueue::Impl> {
 public:
  Impl(std::chrono::milliseconds cache_duration,
       std::shared_ptr<toolkit::EventPoller> poller)
      : poller_(poller ? std::move(poller)
                       : toolkit::EventPollerPool::Instance().getPoller()),
        cache_duration_us_(
            std::chrono::duration_cast<std::chrono::microseconds>(
                cache_duration)
                .count()) {
    validateCacheDuration(cache_duration);
  }

  void setOnPacket(OnPacket callback) {
    dispatch([self = shared_from_this(),
              callback = std::move(callback)]() mutable {
      self->on_packet_ = std::move(callback);
    });
  }

  void setOnState(OnState callback) {
    dispatch([self = shared_from_this(),
              callback = std::move(callback)]() mutable {
      self->on_state_ = std::move(callback);
    });
  }

  void setOnTimelineReset(OnTimelineReset callback) {
    dispatch([self = shared_from_this(),
              callback = std::move(callback)]() mutable {
      self->on_timeline_reset_ = std::move(callback);
    });
  }

  void setStreams(std::uint64_t generation,
                  std::vector<PacketStream> streams) {
    dispatch([self = shared_from_this(), generation,
              streams = std::move(streams)]() mutable {
      self->setStreamsOnPoller(generation, std::move(streams));
    });
  }

  bool input(std::uint64_t generation, const AVPacket* packet) {
    if (disposing_.load(std::memory_order_acquire) || !packet ||
        packet->dts == AV_NOPTS_VALUE || packet->stream_index < 0 ||
        generation < this->generation() ||
        state() == PacketQueueState::Stopped) {
      return false;
    }

    auto owned = clonePacket(packet);
    if (!owned) {
      return false;
    }

    dispatch([self = shared_from_this(), generation,
              packet = std::move(owned)]() mutable {
      self->inputOnPoller(generation, std::move(packet));
    });
    return true;
  }

  void endInput(std::uint64_t generation) {
    dispatch([self = shared_from_this(), generation]() {
      self->endInputOnPoller(generation);
    });
  }

  void pause(bool paused) {
    dispatch([self = shared_from_this(), paused]() {
      self->pauseOnPoller(paused);
    });
  }

  void setPlaybackRate(double rate) {
    dispatch([self = shared_from_this(), rate]() {
      self->setPlaybackRateOnPoller(rate);
    });
  }

  void stop() {
    dispatch([self = shared_from_this()]() { self->stopOnPoller(); });
  }

  void dispose() {
    if (disposing_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (poller_->isCurrentThread()) {
      disposeOnPoller();
      return;
    }

    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();
    poller_->async(
        [self = shared_from_this(), completed]() {
          self->disposeOnPoller();
          completed->set_value();
        },
        false);
    future.wait();
  }

  PacketQueueState state() const noexcept {
    return state_.load(std::memory_order_relaxed);
  }

  std::uint64_t generation() const noexcept {
    return generation_.load(std::memory_order_relaxed);
  }

  std::size_t packetCount() const noexcept {
    return packet_count_.load(std::memory_order_relaxed);
  }

  const std::shared_ptr<toolkit::EventPoller>& getPoller() const {
    return poller_;
  }

 private:
  struct CachedPacket {
    PacketPtr packet;
    std::int64_t dts_us = AV_NOPTS_VALUE;
  };

  struct TrackQueue {
    PacketStream stream;
    std::deque<CachedPacket> packets;
    std::int64_t latest_dts_us = AV_NOPTS_VALUE;

    void clear() {
      packets.clear();
      latest_dts_us = AV_NOPTS_VALUE;
    }
  };

  template <typename Func>
  void dispatch(Func&& function) {
    if (poller_->isCurrentThread()) {
      function();
      return;
    }
    poller_->async(std::forward<Func>(function), false);
  }

  void setStreamsOnPoller(std::uint64_t generation,
                          std::vector<PacketStream> streams) {
    if (disposing_.load(std::memory_order_acquire) || generation == 0 ||
        generation < this->generation()) {
      return;
    }
    if (streams_configured_ && generation == this->generation()) {
      return;
    }

    const bool timeline_changed =
        generation_initialized_ && generation != this->generation();
    const auto previous_epoch = output_epoch_;
    resetTimelineOnPoller(generation);
    const auto reset_epoch = previous_epoch + 1;
    if (disposing_.load(std::memory_order_acquire) ||
        output_epoch_ != reset_epoch ||
        state() != PacketQueueState::Filling ||
        generation != this->generation()) {
      return;
    }

    for (const auto& stream : streams) {
      if (stream.media_type == AVMEDIA_TYPE_AUDIO) {
        audio_.stream = stream;
      } else {
        video_.stream = stream;
      }
    }
    streams_configured_ = true;

    if (timeline_changed && on_timeline_reset_) {
      auto callback = on_timeline_reset_;
      callback(generation);
    }
  }

  void inputOnPoller(std::uint64_t generation, PacketPtr packet) {
    if (disposing_.load(std::memory_order_acquire) || !streams_configured_ ||
        state() == PacketQueueState::Stopped ||
        generation < this->generation() ||
        (generation == this->generation() && input_ended_)) {
      return;
    }

    if (generation > this->generation()) {
      const auto previous_epoch = output_epoch_;
      resetTimelineOnPoller(generation);
      const auto reset_epoch = previous_epoch + 1;
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch_ != reset_epoch ||
          state() != PacketQueueState::Filling ||
          generation != this->generation()) {
        return;
      }
      if (on_timeline_reset_) {
        auto callback = on_timeline_reset_;
        callback(generation);
      }
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch_ != reset_epoch ||
          state() != PacketQueueState::Filling ||
          generation != this->generation()) {
        return;
      }
    }

    auto* track = findTrack(packet->stream_index);
    if (!track) {
      return;
    }

    const auto dts_us =
        av_rescale_q(packet->dts, track->stream.time_base,
                     kComparisonTimeBase);
    if (track->latest_dts_us != AV_NOPTS_VALUE &&
        dts_us < track->latest_dts_us) {
      return;
    }

    track->latest_dts_us = dts_us;
    track->packets.push_back({std::move(packet), dts_us});
    updatePacketCountOnPoller();

    if (state() == PacketQueueState::Filling ||
        state() == PacketQueueState::Starved) {
      startOutputIfReadyOnPoller();
    }
  }

  void endInputOnPoller(std::uint64_t generation) {
    if (disposing_.load(std::memory_order_acquire) || !streams_configured_ ||
        generation != this->generation() ||
        state() == PacketQueueState::Stopped) {
      return;
    }

    input_ended_ = true;
    if ((state() == PacketQueueState::Filling ||
         state() == PacketQueueState::Starved) &&
        nextPacket()) {
      startOutputOnPoller();
    }
  }

  void pauseOnPoller(bool paused) {
    if (disposing_.load(std::memory_order_acquire) ||
        state() == PacketQueueState::Stopped ||
        paused == (state() == PacketQueueState::Paused)) {
      return;
    }

    if (paused) {
      resume_state_ = state();
      if (state() == PacketQueueState::Playing) {
        clock_media_us_ = mediaNowUs(Clock::now());
        clock_wall_ = Clock::now();
      }
      cancelOutputTaskOnPoller();
      ++output_epoch_;
      notifyStateOnPoller(PacketQueueState::Paused);
      return;
    }

    if (resume_state_ == PacketQueueState::Playing && nextPacket()) {
      const auto output_epoch = output_epoch_;
      clock_wall_ = Clock::now();
      notifyStateOnPoller(PacketQueueState::Playing);
      if (output_epoch == output_epoch_ &&
          state() == PacketQueueState::Playing) {
        scheduleNextPacketOnPoller();
      }
      return;
    }

    if (input_ended_ && nextPacket()) {
      startOutputOnPoller();
    } else {
      const auto output_epoch = output_epoch_;
      notifyStateOnPoller(PacketQueueState::Filling);
      if (output_epoch == output_epoch_ &&
          state() == PacketQueueState::Filling) {
        startOutputIfReadyOnPoller();
      }
    }
  }

  void setPlaybackRateOnPoller(double rate) {
    if (disposing_.load(std::memory_order_acquire) ||
        state() == PacketQueueState::Stopped || playback_rate_ == rate) {
      return;
    }

    if (state() == PacketQueueState::Playing) {
      clock_media_us_ = mediaNowUs(Clock::now());
      clock_wall_ = Clock::now();
      cancelOutputTaskOnPoller();
      ++output_epoch_;
      playback_rate_ = rate;
      scheduleNextPacketOnPoller();
      return;
    }
    playback_rate_ = rate;
  }

  TrackQueue* findTrack(int stream_index) {
    if (audio_.stream.stream_index == stream_index) {
      return &audio_;
    }
    if (video_.stream.stream_index == stream_index) {
      return &video_;
    }
    return nullptr;
  }

  bool trackReady(const TrackQueue& track) const {
    return track.packets.size() >= 2 &&
           track.packets.back().dts_us - track.packets.front().dts_us >=
               cache_duration_us_;
  }

  void startOutputIfReadyOnPoller() {
    if (!trackReady(audio_) || !trackReady(video_)) {
      notifyStateOnPoller(PacketQueueState::Filling);
      return;
    }

    startOutputOnPoller();
  }

  void startOutputOnPoller() {
    const auto* next = nextPacket();
    if (!next) {
      notifyStateOnPoller(PacketQueueState::Starved);
      return;
    }

    clock_media_us_ = next->dts_us;
    clock_wall_ = Clock::now();
    const auto output_epoch = output_epoch_;
    notifyStateOnPoller(PacketQueueState::Playing);
    if (output_epoch == output_epoch_ &&
        state() == PacketQueueState::Playing) {
      scheduleNextPacketOnPoller();
    }
  }

  const CachedPacket* nextPacket() const {
    if (audio_.packets.empty()) {
      return input_ended_ && !video_.packets.empty()
                 ? &video_.packets.front()
                 : nullptr;
    }
    if (video_.packets.empty()) {
      return input_ended_ ? &audio_.packets.front() : nullptr;
    }

    const auto& audio = audio_.packets.front();
    const auto& video = video_.packets.front();
    if (audio.dts_us != video.dts_us) {
      return audio.dts_us < video.dts_us ? &audio : &video;
    }
    return audio.packet->stream_index < video.packet->stream_index ? &audio
                                                                   : &video;
  }

  TrackQueue& nextTrack() {
    if (audio_.packets.empty()) {
      return video_;
    }
    if (video_.packets.empty()) {
      return audio_;
    }

    const auto& audio = audio_.packets.front();
    const auto& video = video_.packets.front();
    if (audio.dts_us != video.dts_us) {
      return audio.dts_us < video.dts_us ? audio_ : video_;
    }
    return audio.packet->stream_index < video.packet->stream_index ? audio_
                                                                   : video_;
  }

  std::int64_t mediaNowUs(Clock::time_point now) const {
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - clock_wall_)
            .count();
    return clock_media_us_ + static_cast<std::int64_t>(
                                 std::llround(elapsed_us * playback_rate_));
  }

  void scheduleNextPacketOnPoller() {
    cancelOutputTaskOnPoller();

    const auto* next = nextPacket();
    if (!next) {
      notifyStateOnPoller(PacketQueueState::Starved);
      return;
    }

    const auto remaining_us =
        std::max<std::int64_t>(0, next->dts_us - mediaNowUs(Clock::now()));
    const auto remaining_wall_us =
        static_cast<std::int64_t>(std::ceil(remaining_us / playback_rate_));
    const auto delay_ms = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>((remaining_wall_us + 999) / 1000));
    const auto output_epoch = output_epoch_;
    std::weak_ptr<Impl> weak_self = shared_from_this();
    output_task_ = poller_->doDelayTask(delay_ms, [weak_self, output_epoch]() {
      if (auto self = weak_self.lock();
          self && !self->disposing_.load(std::memory_order_acquire) &&
          output_epoch == self->output_epoch_ &&
          self->state() == PacketQueueState::Playing) {
        self->outputDuePacketsOnPoller();
      }
      return std::uint64_t{0};
    });
  }

  void outputDuePacketsOnPoller() {
    output_task_.reset();
    const auto output_epoch = output_epoch_;
    const auto output_generation = generation();
    const auto media_now_us = mediaNowUs(Clock::now());

    while (const auto* next = nextPacket()) {
      if (next->dts_us > media_now_us) {
        break;
      }

      auto& track = nextTrack();
      auto cached = std::move(track.packets.front());
      track.packets.pop_front();
      updatePacketCountOnPoller();

      if (on_packet_ &&
          !disposing_.load(std::memory_order_acquire)) {
        auto callback = on_packet_;
        callback(output_generation, cached.packet.get());
      }
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch != output_epoch_ ||
          output_generation != generation() ||
          state() != PacketQueueState::Playing) {
        return;
      }
    }

    if (state() != PacketQueueState::Playing) {
      return;
    }
    if (nextPacket()) {
      scheduleNextPacketOnPoller();
    } else {
      notifyStateOnPoller(PacketQueueState::Starved);
    }
  }

  void resetTimelineOnPoller(std::uint64_t generation) {
    cancelOutputTaskOnPoller();
    ++output_epoch_;
    audio_.clear();
    video_.clear();
    packet_count_.store(0, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_relaxed);
    generation_initialized_ = true;
    clock_media_us_ = 0;
    clock_wall_ = {};
    input_ended_ = false;
    resume_state_ = PacketQueueState::Filling;
    notifyStateOnPoller(PacketQueueState::Filling);
  }

  void cancelOutputTaskOnPoller() {
    if (output_task_) {
      output_task_->cancel();
      output_task_.reset();
    }
  }

  void updatePacketCountOnPoller() {
    packet_count_.store(audio_.packets.size() + video_.packets.size(),
                        std::memory_order_relaxed);
  }

  void stopOnPoller() {
    cancelOutputTaskOnPoller();
    ++output_epoch_;
    audio_.clear();
    video_.clear();
    packet_count_.store(0, std::memory_order_relaxed);
    streams_configured_ = false;
    input_ended_ = false;
    notifyStateOnPoller(PacketQueueState::Stopped);
  }

  void disposeOnPoller() {
    on_packet_ = nullptr;
    on_state_ = nullptr;
    on_timeline_reset_ = nullptr;
    stopOnPoller();
  }

  void notifyStateOnPoller(PacketQueueState new_state) {
    if (state() == new_state) {
      return;
    }
    state_.store(new_state, std::memory_order_relaxed);
    if (on_state_ && !disposing_.load(std::memory_order_acquire)) {
      auto callback = on_state_;
      callback(generation(), new_state);
    }
  }

  std::shared_ptr<toolkit::EventPoller> poller_;
  std::int64_t cache_duration_us_;
  TrackQueue audio_;
  TrackQueue video_;
  toolkit::EventPoller::DelayTask::Ptr output_task_;
  std::uint64_t output_epoch_ = 0;
  Clock::time_point clock_wall_;
  std::int64_t clock_media_us_ = 0;
  double playback_rate_ = 1.0;
  bool generation_initialized_ = false;
  bool streams_configured_ = false;
  bool input_ended_ = false;
  PacketQueueState resume_state_ = PacketQueueState::Filling;

  OnPacket on_packet_;
  OnState on_state_;
  OnTimelineReset on_timeline_reset_;

  std::atomic<PacketQueueState> state_{PacketQueueState::Filling};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::size_t> packet_count_{0};
  std::atomic_bool disposing_{false};
};

PacketQueue::PacketQueue(std::chrono::milliseconds cache_duration,
                         std::shared_ptr<toolkit::EventPoller> poller)
    : impl_(std::make_shared<Impl>(cache_duration, std::move(poller))) {}

PacketQueue::~PacketQueue() {
  if (impl_) {
    impl_->dispose();
  }
}

void PacketQueue::setOnPacket(OnPacket callback) {
  impl_->setOnPacket(std::move(callback));
}

void PacketQueue::setOnState(OnState callback) {
  impl_->setOnState(std::move(callback));
}

void PacketQueue::setOnTimelineReset(OnTimelineReset callback) {
  impl_->setOnTimelineReset(std::move(callback));
}

void PacketQueue::setStreams(std::uint64_t generation,
                             std::vector<PacketStream> streams) {
  validateStreams(streams);
  impl_->setStreams(generation, std::move(streams));
}

bool PacketQueue::input(std::uint64_t generation, const AVPacket* packet) {
  return impl_->input(generation, packet);
}

void PacketQueue::endInput(std::uint64_t generation) {
  impl_->endInput(generation);
}

void PacketQueue::pause(bool paused) { impl_->pause(paused); }

void PacketQueue::setPlaybackRate(double rate) {
  if (!std::isfinite(rate) || rate < 0.1 || rate > 20.0) {
    throw std::invalid_argument("rate必须在0.1到20之间");
  }
  impl_->setPlaybackRate(rate);
}

void PacketQueue::stop() { impl_->stop(); }

PacketQueueState PacketQueue::state() const noexcept { return impl_->state(); }

std::uint64_t PacketQueue::generation() const noexcept {
  return impl_->generation();
}

std::size_t PacketQueue::packetCount() const noexcept {
  return impl_->packetCount();
}

std::shared_ptr<toolkit::EventPoller> PacketQueue::getPoller() const {
  return impl_->getPoller();
}

}  // namespace mw::cache

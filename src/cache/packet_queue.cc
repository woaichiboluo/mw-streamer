#include "mw/cache/packet_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "Poller/EventPoller.h"

namespace mw::streamer::cache {
namespace {

using Clock = std::chrono::steady_clock;

constexpr AVRational kComparisonTimeBase{1, AV_TIME_BASE};

void ValidateCacheDuration(std::chrono::milliseconds duration) {
  using namespace std::chrono_literals;
  if (duration != 0ms && (duration < 1s || duration > 30s)) {
    throw std::invalid_argument("cache_duration必须为0，或在1秒到30秒之间");
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
    ValidateCacheDuration(cache_duration);
  }

  void SetOnPacket(OnPacket callback) {
    Dispatch(
        [self = shared_from_this(), callback = std::move(callback)]() mutable {
          self->on_packet_ = std::move(callback);
        });
  }

  void SetOnState(OnState callback) {
    Dispatch(
        [self = shared_from_this(), callback = std::move(callback)]() mutable {
          self->on_state_ = std::move(callback);
        });
  }

  void SetOnTimelineReset(OnTimelineReset callback) {
    Dispatch(
        [self = shared_from_this(), callback = std::move(callback)]() mutable {
          self->on_timeline_reset_ = std::move(callback);
        });
  }

  void SetOnGenerationEnd(OnGenerationEnd callback) {
    Dispatch(
        [self = shared_from_this(), callback = std::move(callback)]() mutable {
          self->on_generation_end_ = std::move(callback);
        });
  }

  void SetStreams(std::uint64_t generation,
                  const std::vector<ffmpeg::StreamInfo>& streams) {
    auto descriptors = DescribeStreams(streams);
    Dispatch([self = shared_from_this(), generation,
              descriptors = std::move(descriptors)]() mutable {
      self->SetStreamsOnPoller(generation, std::move(descriptors));
    });
  }

  bool Input(std::uint64_t generation, const ffmpeg::Packet& packet) {
    const auto* raw_packet = packet.get();
    if (disposing_.load(std::memory_order_acquire) || !raw_packet ||
        raw_packet->dts == AV_NOPTS_VALUE || raw_packet->stream_index < 0 ||
        generation < this->generation() ||
        state() == PacketQueueState::kStopped) {
      return false;
    }

    try {
      auto owned = packet.Ref();
      Dispatch([self = shared_from_this(), generation,
                packet = std::move(owned)]() mutable {
        self->InputOnPoller(generation, std::move(packet));
      });
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

  void EndInput(std::uint64_t generation) {
    Dispatch([self = shared_from_this(), generation]() {
      self->EndInputOnPoller(generation);
    });
  }

  void Pause(bool paused) {
    Dispatch(
        [self = shared_from_this(), paused]() { self->PauseOnPoller(paused); });
  }

  void SetPlaybackRate(double rate) {
    Dispatch([self = shared_from_this(), rate]() {
      self->SetPlaybackRateOnPoller(rate);
    });
  }

  void Stop() {
    Dispatch([self = shared_from_this()]() { self->StopOnPoller(); });
  }

  void Dispose() {
    if (disposing_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (poller_->isCurrentThread()) {
      DisposeOnPoller();
      return;
    }

    auto completed = std::make_shared<std::promise<void>>();
    auto future = completed->get_future();
    poller_->async(
        [self = shared_from_this(), completed]() {
          self->DisposeOnPoller();
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

  std::size_t packet_count() const noexcept {
    return packet_count_.load(std::memory_order_relaxed);
  }

  const std::shared_ptr<toolkit::EventPoller>& poller() const {
    return poller_;
  }

 private:
  struct StreamDescriptor {
    int stream_index = -1;
    AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
    AVRational time_base{0, 1};
  };

  struct CachedPacket {
    ffmpeg::Packet packet;
    std::int64_t dts_us = AV_NOPTS_VALUE;
  };

  struct TrackQueue {
    int stream_index = -1;
    AVRational time_base{0, 1};
    std::deque<CachedPacket> packets;
    std::int64_t latest_dts_us = AV_NOPTS_VALUE;

    bool configured() const { return stream_index >= 0; }

    void Clear() {
      packets.clear();
      latest_dts_us = AV_NOPTS_VALUE;
    }

    void Reset() {
      stream_index = -1;
      time_base = {0, 1};
      Clear();
    }
  };

  template <typename Func>
  void Dispatch(Func&& function) {
    if (poller_->isCurrentThread()) {
      function();
      return;
    }
    poller_->async(std::forward<Func>(function), false);
  }

  static std::vector<StreamDescriptor> DescribeStreams(
      const std::vector<ffmpeg::StreamInfo>& streams) {
    std::vector<StreamDescriptor> descriptors;
    descriptors.reserve(streams.size());
    int audio_stream_index = -1;
    int video_stream_index = -1;

    for (const auto& stream : streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (!parameters) {
        throw std::invalid_argument("FFmpeg StreamInfo参数无效");
      }
      if (parameters->codec_type != AVMEDIA_TYPE_AUDIO &&
          parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }

      stream.Validate();
      if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (audio_stream_index >= 0) {
          throw std::invalid_argument("PacketQueue最多支持一路音频和一路视频");
        }
        audio_stream_index = stream.stream_index;
      } else {
        if (video_stream_index >= 0) {
          throw std::invalid_argument("PacketQueue最多支持一路音频和一路视频");
        }
        video_stream_index = stream.stream_index;
      }
      descriptors.push_back(
          {stream.stream_index, parameters->codec_type, stream.time_base});
    }

    if (descriptors.empty()) {
      throw std::invalid_argument("PacketQueue至少需要一路音频或视频");
    }
    if (audio_stream_index >= 0 && audio_stream_index == video_stream_index) {
      throw std::invalid_argument("音频和视频stream_index不能相同");
    }
    return descriptors;
  }

  void SetStreamsOnPoller(std::uint64_t generation,
                          std::vector<StreamDescriptor> streams) {
    if (disposing_.load(std::memory_order_acquire) || generation == 0 ||
        generation < this->generation()) {
      return;
    }
    if (streams_configured_ && generation == this->generation()) {
      return;
    }

    const bool timeline_changed =
        generation_initialized_ && generation != this->generation();
    streams_configured_ = false;
    audio_.Reset();
    video_.Reset();
    const auto previous_epoch = output_epoch_;
    ResetTimelineOnPoller(generation);
    const auto reset_epoch = previous_epoch + 1;
    if (disposing_.load(std::memory_order_acquire) ||
        output_epoch_ != reset_epoch || state() != PacketQueueState::kFilling ||
        generation != this->generation()) {
      return;
    }

    for (const auto& stream : streams) {
      if (stream.media_type == AVMEDIA_TYPE_AUDIO) {
        audio_.stream_index = stream.stream_index;
        audio_.time_base = stream.time_base;
      } else {
        video_.stream_index = stream.stream_index;
        video_.time_base = stream.time_base;
      }
    }
    streams_configured_ = true;

    if (timeline_changed && on_timeline_reset_) {
      auto callback = on_timeline_reset_;
      callback(generation);
    }
  }

  void InputOnPoller(std::uint64_t generation, ffmpeg::Packet packet) {
    if (disposing_.load(std::memory_order_acquire) || !streams_configured_ ||
        state() == PacketQueueState::kStopped ||
        generation < this->generation() ||
        (generation == this->generation() && input_ended_)) {
      return;
    }

    if (generation > this->generation()) {
      const auto previous_epoch = output_epoch_;
      ResetTimelineOnPoller(generation);
      const auto reset_epoch = previous_epoch + 1;
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch_ != reset_epoch ||
          state() != PacketQueueState::kFilling ||
          generation != this->generation()) {
        return;
      }
      if (on_timeline_reset_) {
        auto callback = on_timeline_reset_;
        callback(generation);
      }
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch_ != reset_epoch ||
          state() != PacketQueueState::kFilling ||
          generation != this->generation()) {
        return;
      }
    }

    const auto* raw_packet = packet.get();
    auto* track = FindTrack(raw_packet->stream_index);
    if (!track) {
      return;
    }

    const auto dts_us =
        av_rescale_q(raw_packet->dts, track->time_base, kComparisonTimeBase);
    if (track->latest_dts_us != AV_NOPTS_VALUE &&
        dts_us < track->latest_dts_us) {
      return;
    }

    track->latest_dts_us = dts_us;

    if (IsImmediateForwarding()) {
      if (state() == PacketQueueState::kPaused) {
        return;
      }

      const auto output_epoch = output_epoch_;
      NotifyStateOnPoller(PacketQueueState::kPlaying);
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch != output_epoch_ || generation != this->generation() ||
          state() != PacketQueueState::kPlaying) {
        return;
      }
      if (on_packet_) {
        auto callback = on_packet_;
        callback(generation, packet);
      }
      return;
    }

    track->packets.push_back({std::move(packet), dts_us});
    UpdatePacketCountOnPoller();

    if (state() == PacketQueueState::kFilling ||
        state() == PacketQueueState::kStarved) {
      StartOutputIfReadyOnPoller();
    }
  }

  void EndInputOnPoller(std::uint64_t generation) {
    if (disposing_.load(std::memory_order_acquire) || !streams_configured_ ||
        generation != this->generation() ||
        state() == PacketQueueState::kStopped) {
      return;
    }

    input_ended_ = true;
    if (IsImmediateForwarding()) {
      NotifyStarvedIfEmptyOnPoller(generation);
      return;
    }

    if ((state() == PacketQueueState::kFilling ||
         state() == PacketQueueState::kStarved) &&
        NextPacket()) {
      StartOutputOnPoller();
      return;
    }
    if (!NextPacket()) {
      NotifyStarvedIfEmptyOnPoller(generation);
    }
  }

  void PauseOnPoller(bool paused) {
    if (disposing_.load(std::memory_order_acquire) ||
        state() == PacketQueueState::kStopped ||
        paused == (state() == PacketQueueState::kPaused)) {
      return;
    }

    if (IsImmediateForwarding()) {
      if (paused) {
        resume_state_ = state();
        ++output_epoch_;
        NotifyStateOnPoller(PacketQueueState::kPaused);
      } else if (input_ended_) {
        NotifyStateOnPoller(PacketQueueState::kStarved);
      } else {
        NotifyStateOnPoller(resume_state_ == PacketQueueState::kPlaying
                                ? PacketQueueState::kPlaying
                                : PacketQueueState::kFilling);
      }
      return;
    }

    if (paused) {
      resume_state_ = state();
      if (state() == PacketQueueState::kPlaying) {
        clock_media_us_ = MediaNowUs(Clock::now());
        clock_wall_ = Clock::now();
      }
      CancelOutputTaskOnPoller();
      ++output_epoch_;
      NotifyStateOnPoller(PacketQueueState::kPaused);
      return;
    }

    if (resume_state_ == PacketQueueState::kPlaying && NextPacket()) {
      const auto output_epoch = output_epoch_;
      clock_wall_ = Clock::now();
      NotifyStateOnPoller(PacketQueueState::kPlaying);
      if (output_epoch == output_epoch_ &&
          state() == PacketQueueState::kPlaying) {
        ScheduleNextPacketOnPoller();
      }
      return;
    }

    if (input_ended_ && NextPacket()) {
      StartOutputOnPoller();
    } else {
      const auto output_epoch = output_epoch_;
      NotifyStateOnPoller(PacketQueueState::kFilling);
      if (output_epoch == output_epoch_ &&
          state() == PacketQueueState::kFilling) {
        StartOutputIfReadyOnPoller();
      }
    }
  }

  void SetPlaybackRateOnPoller(double rate) {
    if (disposing_.load(std::memory_order_acquire) ||
        state() == PacketQueueState::kStopped || playback_rate_ == rate) {
      return;
    }

    if (IsImmediateForwarding()) {
      playback_rate_ = rate;
      return;
    }

    if (state() == PacketQueueState::kPlaying) {
      clock_media_us_ = MediaNowUs(Clock::now());
      clock_wall_ = Clock::now();
      CancelOutputTaskOnPoller();
      ++output_epoch_;
      playback_rate_ = rate;
      ScheduleNextPacketOnPoller();
      return;
    }
    playback_rate_ = rate;
  }

  TrackQueue* FindTrack(int stream_index) {
    if (audio_.configured() && audio_.stream_index == stream_index) {
      return &audio_;
    }
    if (video_.configured() && video_.stream_index == stream_index) {
      return &video_;
    }
    return nullptr;
  }

  bool IsImmediateForwarding() const { return cache_duration_us_ == 0; }

  bool TrackReady(const TrackQueue& track) const {
    return track.packets.size() >= 2 &&
           track.packets.back().dts_us - track.packets.front().dts_us >=
               cache_duration_us_;
  }

  void StartOutputIfReadyOnPoller() {
    if ((audio_.configured() && !TrackReady(audio_)) ||
        (video_.configured() && !TrackReady(video_))) {
      NotifyStateOnPoller(PacketQueueState::kFilling);
      return;
    }

    StartOutputOnPoller();
  }

  void StartOutputOnPoller() {
    AlignTrackFrontsOnPoller();
    const auto* next = NextPacket();
    if (!next) {
      NotifyStarvedIfEmptyOnPoller(generation());
      return;
    }

    clock_media_us_ = next->dts_us;
    clock_wall_ = Clock::now();
    const auto output_epoch = output_epoch_;
    NotifyStateOnPoller(PacketQueueState::kPlaying);
    if (output_epoch == output_epoch_ &&
        state() == PacketQueueState::kPlaying) {
      ScheduleNextPacketOnPoller();
    }
  }

  void AlignTrackFrontsOnPoller() {
    if (!audio_.configured() || !video_.configured() ||
        audio_.packets.empty() || video_.packets.empty()) {
      return;
    }

    const auto common_dts_us =
        std::max(audio_.packets.front().dts_us, video_.packets.front().dts_us);
    TrimLeadingPackets(audio_, common_dts_us);
    TrimLeadingPackets(video_, common_dts_us);
    UpdatePacketCountOnPoller();
  }

  static void TrimLeadingPackets(TrackQueue& track,
                                 std::int64_t common_dts_us) {
    while (track.packets.size() > 1 &&
           track.packets[1].dts_us <= common_dts_us) {
      track.packets.pop_front();
    }
  }

  const CachedPacket* NextPacket() const {
    if (!audio_.configured()) {
      return video_.packets.empty() ? nullptr : &video_.packets.front();
    }
    if (!video_.configured()) {
      return audio_.packets.empty() ? nullptr : &audio_.packets.front();
    }
    if (audio_.packets.empty()) {
      return input_ended_ && !video_.packets.empty() ? &video_.packets.front()
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
    return audio.packet.get()->stream_index < video.packet.get()->stream_index
               ? &audio
               : &video;
  }

  TrackQueue& NextTrack() {
    if (!audio_.configured()) {
      return video_;
    }
    if (!video_.configured()) {
      return audio_;
    }
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
    return audio.packet.get()->stream_index < video.packet.get()->stream_index
               ? audio_
               : video_;
  }

  std::int64_t MediaNowUs(Clock::time_point now) const {
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - clock_wall_)
            .count();
    return clock_media_us_ +
           static_cast<std::int64_t>(std::llround(elapsed_us * playback_rate_));
  }

  void ScheduleNextPacketOnPoller() {
    CancelOutputTaskOnPoller();

    const auto* next = NextPacket();
    if (!next) {
      NotifyStarvedIfEmptyOnPoller(generation());
      return;
    }

    const auto remaining_us =
        std::max<std::int64_t>(0, next->dts_us - MediaNowUs(Clock::now()));
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
          self->state() == PacketQueueState::kPlaying) {
        self->OutputDuePacketsOnPoller();
      }
      return std::uint64_t{0};
    });
  }

  void OutputDuePacketsOnPoller() {
    output_task_.reset();
    const auto output_epoch = output_epoch_;
    const auto output_generation = generation();
    const auto media_now_us = MediaNowUs(Clock::now());

    while (const auto* next = NextPacket()) {
      if (next->dts_us > media_now_us) {
        break;
      }

      auto& track = NextTrack();
      auto cached = std::move(track.packets.front());
      track.packets.pop_front();
      UpdatePacketCountOnPoller();

      if (on_packet_ && !disposing_.load(std::memory_order_acquire)) {
        auto callback = on_packet_;
        callback(output_generation, cached.packet);
      }
      if (disposing_.load(std::memory_order_acquire) ||
          output_epoch != output_epoch_ || output_generation != generation() ||
          state() != PacketQueueState::kPlaying) {
        return;
      }
    }

    if (state() != PacketQueueState::kPlaying) {
      return;
    }
    if (NextPacket()) {
      ScheduleNextPacketOnPoller();
    } else {
      NotifyStarvedIfEmptyOnPoller(output_generation);
    }
  }

  void ResetTimelineOnPoller(std::uint64_t generation) {
    CancelOutputTaskOnPoller();
    ++output_epoch_;
    audio_.Clear();
    video_.Clear();
    packet_count_.store(0, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_relaxed);
    generation_initialized_ = true;
    clock_media_us_ = 0;
    clock_wall_ = {};
    input_ended_ = false;
    generation_ended_ = false;
    resume_state_ = PacketQueueState::kFilling;
    NotifyStateOnPoller(PacketQueueState::kFilling);
  }

  void CancelOutputTaskOnPoller() {
    if (output_task_) {
      output_task_->cancel();
      output_task_.reset();
    }
  }

  void UpdatePacketCountOnPoller() {
    packet_count_.store(audio_.packets.size() + video_.packets.size(),
                        std::memory_order_relaxed);
  }

  void StopOnPoller() {
    CancelOutputTaskOnPoller();
    ++output_epoch_;
    audio_.Reset();
    video_.Reset();
    packet_count_.store(0, std::memory_order_relaxed);
    streams_configured_ = false;
    input_ended_ = false;
    generation_ended_ = false;
    NotifyStateOnPoller(PacketQueueState::kStopped);
  }

  void DisposeOnPoller() {
    on_packet_ = nullptr;
    on_state_ = nullptr;
    on_timeline_reset_ = nullptr;
    on_generation_end_ = nullptr;
    StopOnPoller();
  }

  void NotifyGenerationEndOnPoller(std::uint64_t event_generation) {
    if (disposing_.load(std::memory_order_acquire) || generation_ended_ ||
        !streams_configured_ || !input_ended_ || NextPacket() ||
        event_generation != this->generation() ||
        state() == PacketQueueState::kStopped) {
      return;
    }

    generation_ended_ = true;
    if (on_generation_end_) {
      auto callback = on_generation_end_;
      callback(event_generation);
    }
  }

  void NotifyStarvedIfEmptyOnPoller(std::uint64_t event_generation) {
    if (NextPacket()) {
      return;
    }
    NotifyGenerationEndOnPoller(event_generation);
    if (event_generation == generation() &&
        state() != PacketQueueState::kStopped && !NextPacket()) {
      NotifyStateOnPoller(PacketQueueState::kStarved);
    }
  }

  void NotifyStateOnPoller(PacketQueueState new_state) {
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
  bool generation_ended_ = false;
  PacketQueueState resume_state_ = PacketQueueState::kFilling;

  OnPacket on_packet_;
  OnState on_state_;
  OnTimelineReset on_timeline_reset_;
  OnGenerationEnd on_generation_end_;

  std::atomic<PacketQueueState> state_{PacketQueueState::kFilling};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::size_t> packet_count_{0};
  std::atomic_bool disposing_{false};
};

PacketQueue::PacketQueue(std::chrono::milliseconds cache_duration,
                         std::shared_ptr<toolkit::EventPoller> poller)
    : impl_(std::make_shared<Impl>(cache_duration, std::move(poller))) {}

PacketQueue::~PacketQueue() {
  if (impl_) {
    impl_->Dispose();
  }
}

void PacketQueue::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}

void PacketQueue::SetOnState(OnState callback) {
  impl_->SetOnState(std::move(callback));
}

void PacketQueue::SetOnTimelineReset(OnTimelineReset callback) {
  impl_->SetOnTimelineReset(std::move(callback));
}

void PacketQueue::SetOnGenerationEnd(OnGenerationEnd callback) {
  impl_->SetOnGenerationEnd(std::move(callback));
}

void PacketQueue::SetStreams(std::uint64_t generation,
                             const std::vector<ffmpeg::StreamInfo>& streams) {
  impl_->SetStreams(generation, streams);
}

bool PacketQueue::Input(std::uint64_t generation,
                        const ffmpeg::Packet& packet) {
  return impl_->Input(generation, packet);
}

void PacketQueue::EndInput(std::uint64_t generation) {
  impl_->EndInput(generation);
}

void PacketQueue::Pause(bool paused) { impl_->Pause(paused); }

void PacketQueue::SetPlaybackRate(double rate) {
  if (!std::isfinite(rate) || rate < 0.1 || rate > 20.0) {
    throw std::invalid_argument("rate必须在0.1到20之间");
  }
  impl_->SetPlaybackRate(rate);
}

void PacketQueue::Stop() { impl_->Stop(); }

PacketQueueState PacketQueue::state() const noexcept { return impl_->state(); }

std::uint64_t PacketQueue::generation() const noexcept {
  return impl_->generation();
}

std::size_t PacketQueue::packet_count() const noexcept {
  return impl_->packet_count();
}

std::shared_ptr<toolkit::EventPoller> PacketQueue::poller() const {
  return impl_->poller();
}

}  // namespace mw::streamer::cache

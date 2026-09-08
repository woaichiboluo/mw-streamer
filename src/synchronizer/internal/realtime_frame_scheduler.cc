#include "mw/synchronizer/internal/realtime_frame_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include "mw/common/blocking_queue.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/synchronizer/internal/standby_video_frame.h"

namespace mw::streamer::synchronizer::internal {
namespace {

constexpr AVRational kMicroseconds{1, 1000000};
using Clock = RealtimeFrameScheduler::Clock;

std::int64_t Checked(std::int64_t value) {
  // FFmpeg uses INT64_MIN to report an unrepresentable rescale result. This
  // also excludes AV_NOPTS_VALUE from every derived media/wall timestamp.
  if (value == std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("实时同步时间戳溢出");
  }
  return value;
}

std::int64_t Add(std::int64_t left, std::int64_t right) {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  if ((right > 0 && left > kMax - right) ||
      (right < 0 && left < kMin - right)) {
    throw std::overflow_error("实时同步时间戳相加溢出");
  }
  return Checked(left + right);
}

std::int64_t Subtract(std::int64_t left, std::int64_t right) {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  if ((right > 0 && left < kMin + right) ||
      (right < 0 && left > kMax + right)) {
    throw std::overflow_error("实时同步时间戳相减溢出");
  }
  return Checked(left - right);
}

std::int64_t Rescale(std::int64_t value, AVRational from, AVRational to) {
  if (from.num <= 0 || from.den <= 0 || to.num <= 0 || to.den <= 0) {
    throw std::invalid_argument("实时同步time_base无效");
  }
  return Checked(av_rescale_q_rnd(value, from, to, AV_ROUND_NEAR_INF));
}

std::int64_t WallUs(Clock::time_point now) {
  using Scale = std::ratio_divide<Clock::period, std::micro>;
  return Checked(av_rescale_rnd(now.time_since_epoch().count(), Scale::num,
                                Scale::den, AV_ROUND_ZERO));
}

Clock::time_point WallTime(std::int64_t microseconds) {
  using Scale = std::ratio_divide<std::micro, Clock::period>;
  const auto ticks = Checked(
      av_rescale_rnd(microseconds, Scale::num, Scale::den, AV_ROUND_ZERO));
  return Clock::time_point(Clock::duration(ticks));
}

bool SameStream(const ffmpeg::StreamInfo& left,
                const ffmpeg::StreamInfo& right) {
  const auto* a = left.codec_parameters.get();
  const auto* b = right.codec_parameters.get();
  return left.stream_index == right.stream_index &&
         av_cmp_q(left.time_base, right.time_base) == 0 &&
         a->codec_type == b->codec_type && a->codec_id == b->codec_id &&
         a->width == b->width && a->height == b->height &&
         a->sample_rate == b->sample_rate &&
         av_channel_layout_compare(&a->ch_layout, &b->ch_layout) == 0;
}

ffmpeg::Frame MakeSilence(const ffmpeg::Frame& prototype) {
  ffmpeg::Frame frame;
  frame->format = prototype->format;
  frame->sample_rate = prototype->sample_rate;
  frame->nb_samples = prototype->nb_samples;
  ffmpeg::ThrowIfError(
      av_channel_layout_copy(&frame->ch_layout, &prototype->ch_layout),
      "复制实时同步静音声道布局");
  ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 0),
                       "分配实时同步静音帧");
  ffmpeg::ThrowIfError(
      av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
                             frame->ch_layout.nb_channels,
                             static_cast<AVSampleFormat>(frame->format)),
      "填充实时同步静音帧");
  frame.CopyPropertiesFrom(prototype);
  return frame;
}

void SetTiming(ffmpeg::Frame& frame, AVRational time_base, std::int64_t pts,
               std::int64_t duration) {
  frame->time_base = time_base;
  frame->pts = pts;
  frame->best_effort_timestamp = pts;
  frame->pkt_dts = AV_NOPTS_VALUE;
  frame->duration = duration;
}

}  // namespace

class RealtimeFrameScheduler::Impl final {
 public:
  explicit Impl(SynchronizerSinkConfig config)
      : config_(std::move(config)), standby_video_(config_.standby_image_path) {
    if (config_.frame_queue_capacity == 0 ||
        config_.max_frame_lateness.count() < 0 ||
        config_.standby_timeout.count() < 0 ||
        config_.video_frame_rate.num <= 0 ||
        config_.video_frame_rate.den <= 0) {
      throw std::invalid_argument("实时同步队列、帧率或等待时长无效");
    }
    video_time_base_ = av_inv_q(config_.video_frame_rate);
    video_duration_us_ = Rescale(1, video_time_base_, kMicroseconds);
    if (video_duration_us_ <= 0) {
      throw std::invalid_argument("实时同步视频帧率超过调度时钟精度");
    }
    lateness_us_ =
        Rescale(config_.max_frame_lateness.count(), {1, 1000}, kMicroseconds);
    standby_timeout_us_ =
        Rescale(config_.standby_timeout.count(), {1, 1000}, kMicroseconds);
  }

  void Configure(const media::FrameStreamsReady& streams) {
    bool audio = false;
    bool video = false;
    for (const auto& stream : streams.source_streams) {
      stream.Validate();
      const bool is_audio =
          stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO;
      bool& declared = is_audio ? audio : video;
      if (declared) {
        throw std::invalid_argument("实时同步每类媒体只支持一路轨道");
      }
      declared = true;
    }
    if (!audio && !video) {
      throw std::invalid_argument("实时同步缺少媒体轨道");
    }
    if (configured_) {
      if (streams_.size() != streams.source_streams.size()) {
        throw std::invalid_argument("实时同步不支持改变轨道数量");
      }
      for (const auto& previous : streams_) {
        const auto found = std::find_if(
            streams.source_streams.begin(), streams.source_streams.end(),
            [&](const auto& next) { return SameStream(previous, next); });
        if (found == streams.source_streams.end()) {
          throw std::invalid_argument("实时同步不支持改变源轨道参数");
        }
      }
    }
    if (streams.hardware_context) {
      if (video_.prototype && video_.prototype->get()->hw_frames_ctx &&
          !streams.hardware_context->IsCompatible(*video_.prototype->get())) {
        throw std::invalid_argument("实时同步不支持改变CUDA设备上下文");
      }
      hardware_context_ = *streams.hardware_context;
    } else {
      if (video_.prototype && video_.prototype->get()->hw_frames_ctx) {
        throw std::invalid_argument("实时同步CUDA轨道缺少硬件上下文");
      }
      hardware_context_.reset();
    }
    streams_ = streams.source_streams;
    audio_.declared = audio;
    video_.declared = video;
    configured_ = true;
  }

  void Push(const media::FrameReady& ready, bool audio, Clock::time_point now) {
    auto& track = audio ? audio_ : video_;
    if (!configured_ || !track.declared || finishing_) {
      throw std::logic_error("实时同步收到未声明或已结束轨道的帧");
    }
    ValidateFrame(ready.frame, track, audio);
    const auto pts =
        Rescale(ready.frame->pts, ready.frame->time_base, kMicroseconds);
    if (track.last_source_us && pts < *track.last_source_us) {
      throw std::invalid_argument("实时同步同代次源PTS回退");
    }
    track.last_source_us = pts;
    if (!track.prototype) {
      track.prototype = ready.frame.Ref();
      if (audio) {
        silence_ = MakeSilence(ready.frame);
        audio_time_base_ = {1, ready.frame->sample_rate};
      } else {
        standby_video_.Prepare(
            ready.frame, hardware_context_ ? &*hardware_context_ : nullptr);
        last_video_ = ready.frame.Ref();
      }
    }
    if (track.frames.size() >= config_.frame_queue_capacity) {
      track.frames.TryPop();
    }
    track.frames.Push({ready.frame.Ref(), pts});
    MaybeMapSource(WallUs(now));
  }

  void Reset() {
    audio_.frames.Clear();
    video_.frames.Clear();
    audio_.last_source_us.reset();
    video_.last_source_us.reset();
    source_mapped_ = false;
    finishing_ = false;
    force_next_video_ = true;
    standby_ = started_;
  }

  void Finish() {
    if (!PrototypesReady()) {
      throw std::runtime_error("实时同步声明的轨道结束前没有产生原始帧");
    }
    finishing_ = true;
  }

  std::optional<OutputFrame> TakeReady(Clock::time_point now) {
    deadline_.reset();
    const auto now_us = WallUs(now);
    MaybeMapSource(now_us);
    while (started_ && !finished()) {
      auto output = TakeNext(now_us);
      if (output || deadline_) {
        return output;
      }
      // EOF may discard the selected track's last stale frame. Reconsider
      // the remaining track instead of synthesizing beyond the completed one.
    }
    return std::nullopt;
  }

  std::optional<Clock::time_point> deadline() const { return deadline_; }
  bool finished() const {
    return finishing_ && audio_.frames.size() == 0 && video_.frames.size() == 0;
  }
  bool standby() const { return standby_; }
  std::size_t queue_depth() const {
    return audio_.frames.size() + video_.frames.size();
  }

 private:
  struct TimedFrame {
    ffmpeg::Frame frame;
    std::int64_t source_us;
  };
  struct Track {
    common::BlockingQueue<TimedFrame> frames;
    std::optional<ffmpeg::Frame> prototype;
    std::optional<std::int64_t> last_source_us;
    std::int64_t next_pts = 0;
    bool declared = false;
  };

  std::optional<OutputFrame> TakeNext(std::int64_t now_us) {
    const bool audio = AudioFirst();
    const auto output_us = OutputUs(audio);
    auto due_us = Add(output_wall_anchor_us_,
                      Subtract(output_us, output_media_anchor_us_));
    // Select against the original source slot, but release both tracks only
    // after a fixed arrival buffer. Using the release time for selection would
    // skip the very frames this buffer is meant to retain.
    auto release_us = Add(due_us, lateness_us_);
    // A stalled output worker must not burst every missed synthetic slot.
    // Preserve continuous sample/frame counters but move their wall deadline;
    // the source mapping stays fixed so buffered stale media is still skipped.
    if (Subtract(now_us, release_us) >
        std::max(lateness_us_, SlotDurationUs(audio))) {
      output_wall_anchor_us_ = Subtract(now_us, lateness_us_);
      output_media_anchor_us_ = output_us;
      due_us = output_wall_anchor_us_;
      release_us = now_us;
    }
    if (now_us < release_us) {
      deadline_ = WallTime(release_us);
      return std::nullopt;
    }
    auto selected = SelectFrame(audio, due_us);
    if (!selected && finishing_ &&
        (audio ? audio_.frames.size() : video_.frames.size()) == 0) {
      return std::nullopt;
    }
    return audio ? MakeAudio(std::move(selected), now_us)
                 : MakeVideo(std::move(selected), now_us);
  }

  void ValidateFrame(const ffmpeg::Frame& frame, const Track& track,
                     bool audio) const {
    if (!frame.get() || frame->pts == AV_NOPTS_VALUE ||
        frame->time_base.num <= 0 || frame->time_base.den <= 0) {
      throw std::invalid_argument("实时同步原始帧时间戳无效");
    }
    if (audio) {
      if (frame->sample_rate <= 0 || frame->nb_samples <= 0 ||
          av_channel_layout_check(&frame->ch_layout) != 1 ||
          av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format)) <=
              0 ||
          !frame->extended_data || !frame->extended_data[0]) {
        throw std::invalid_argument("实时同步音频帧无效");
      }
      if (track.prototype &&
          (frame->format != track.prototype->get()->format ||
           frame->sample_rate != track.prototype->get()->sample_rate ||
           av_channel_layout_compare(
               &frame->ch_layout, &track.prototype->get()->ch_layout) != 0)) {
        throw std::invalid_argument("实时同步不支持改变音频格式");
      }
      return;
    }
    if (frame->width <= 0 || frame->height <= 0 || frame->format < 0 ||
        !frame->data[0]) {
      throw std::invalid_argument("实时同步视频帧无效");
    }
    if (frame->hw_frames_ctx &&
        (!hardware_context_ ||
         !hardware_context_->IsCompatible(*frame.get()))) {
      throw std::invalid_argument("实时同步CUDA帧与硬件上下文不兼容");
    }
    if (track.prototype &&
        (frame->width != track.prototype->get()->width ||
         frame->height != track.prototype->get()->height ||
         frame->format != track.prototype->get()->format ||
         static_cast<bool>(frame->hw_frames_ctx) !=
             static_cast<bool>(track.prototype->get()->hw_frames_ctx))) {
      throw std::invalid_argument("实时同步不支持改变视频格式或尺寸");
    }
  }

  bool PrototypesReady() const {
    return configured_ && (!audio_.declared || audio_.prototype) &&
           (!video_.declared || video_.prototype);
  }

  void MaybeMapSource(std::int64_t now_us) {
    if (source_mapped_ || !PrototypesReady()) {
      return;
    }
    const auto audio = audio_.frames.TryPeek();
    const auto video = video_.frames.TryPeek();
    if ((!finishing_ &&
         ((audio_.declared && !audio) || (video_.declared && !video))) ||
        (!audio && !video)) {
      return;
    }
    // Startup may have evicted a long prefix while waiting for the other
    // track. Anchor at their retained overlap, never at discarded history.
    source_origin_us_ = audio ? audio->source_us : video->source_us;
    if (video) {
      source_origin_us_ = std::max(source_origin_us_, video->source_us);
    }
    source_wall_anchor_us_ = now_us;
    source_mapped_ = true;
    if (!started_) {
      output_wall_anchor_us_ = now_us;
      output_media_anchor_us_ = 0;
      last_real_video_us_ = now_us;
      last_real_audio_us_ = now_us;
      started_ = true;
    }
  }

  bool AudioFirst() const {
    if (finishing_) {
      if (audio_.frames.size() == 0) {
        return false;
      }
      if (video_.frames.size() == 0) {
        return true;
      }
    }
    return audio_.declared &&
           (!video_.declared || OutputUs(true) <= OutputUs(false));
  }

  std::int64_t OutputUs(bool audio) const {
    return Rescale(audio ? audio_.next_pts : video_.next_pts,
                   audio ? audio_time_base_ : video_time_base_, kMicroseconds);
  }

  std::int64_t SlotDurationUs(bool audio) const {
    return audio ? Rescale(silence_->get()->nb_samples, audio_time_base_,
                           kMicroseconds)
                 : video_duration_us_;
  }

  std::optional<ffmpeg::Frame> SelectFrame(bool audio, std::int64_t due_us) {
    if (!source_mapped_) {
      return std::nullopt;
    }
    const auto target =
        Add(source_origin_us_, Subtract(due_us, source_wall_anchor_us_));
    const auto earliest = Subtract(target, lateness_us_);
    const auto latest = Add(target, SlotDurationUs(audio) / 2);
    auto& queue = audio ? audio_.frames : video_.frames;
    std::optional<ffmpeg::Frame> selected;
    while (const auto head = queue.TryPeek()) {
      if (head->source_us > latest) {
        break;
      }
      auto popped = queue.TryPop();
      if (popped->source_us >= earliest) {
        selected = std::move(popped->frame);
      }
    }
    return selected;
  }

  OutputFrame MakeAudio(std::optional<ffmpeg::Frame> selected,
                        std::int64_t now_us) {
    if (selected) {
      last_real_audio_us_ = now_us;
    }
    auto frame = selected ? std::move(*selected) : silence_->Ref();
    if (!video_.declared) {
      standby_ = !source_mapped_ ||
                 Subtract(now_us, last_real_audio_us_) >= standby_timeout_us_;
    }
    SetTiming(frame, audio_time_base_, audio_.next_pts, frame->nb_samples);
    audio_.next_pts = Add(audio_.next_pts, frame->nb_samples);
    return {true, std::move(frame)};
  }

  OutputFrame MakeVideo(std::optional<ffmpeg::Frame> selected,
                        std::int64_t now_us) {
    const bool was_standby = standby_;
    if (selected) {
      last_real_video_us_ = now_us;
      last_video_ = selected->Ref();
      standby_ = false;
    } else if (!source_mapped_ ||
               Subtract(now_us, last_real_video_us_) >= standby_timeout_us_) {
      standby_ = true;
    }
    auto frame = selected
                     ? std::move(*selected)
                     : (standby_ ? standby_video_.Ref() : last_video_->Ref());
    const bool force =
        std::exchange(force_next_video_, false) || was_standby != standby_;
    frame->pict_type = force ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame->key_frame = force ? 1 : 0;
    if (force) {
      frame->flags |= AV_FRAME_FLAG_KEY;
    } else {
      frame->flags &= ~AV_FRAME_FLAG_KEY;
    }
    SetTiming(frame, video_time_base_, video_.next_pts, 1);
    video_.next_pts = Add(video_.next_pts, 1);
    return {false, std::move(frame)};
  }

  const SynchronizerSinkConfig config_;
  std::vector<ffmpeg::StreamInfo> streams_;
  std::optional<ffmpeg::HardwareContext> hardware_context_;
  Track audio_;
  Track video_;
  std::optional<ffmpeg::Frame> silence_;
  std::optional<ffmpeg::Frame> last_video_;
  StandbyVideoFrame standby_video_;
  AVRational audio_time_base_{0, 1};
  AVRational video_time_base_{0, 1};
  std::int64_t video_duration_us_ = 0;
  std::int64_t lateness_us_ = 0;
  std::int64_t standby_timeout_us_ = 0;
  std::int64_t source_origin_us_ = 0;
  std::int64_t source_wall_anchor_us_ = 0;
  std::int64_t output_media_anchor_us_ = 0;
  std::int64_t output_wall_anchor_us_ = 0;
  std::int64_t last_real_audio_us_ = 0;
  std::int64_t last_real_video_us_ = 0;
  std::optional<Clock::time_point> deadline_;
  bool configured_ = false;
  bool started_ = false;
  bool source_mapped_ = false;
  bool finishing_ = false;
  bool standby_ = false;
  bool force_next_video_ = true;
};

RealtimeFrameScheduler::RealtimeFrameScheduler(SynchronizerSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
RealtimeFrameScheduler::~RealtimeFrameScheduler() = default;
void RealtimeFrameScheduler::Configure(
    const media::FrameStreamsReady& streams) {
  impl_->Configure(streams);
}
void RealtimeFrameScheduler::Push(const media::FrameReady& frame, bool audio,
                                  Clock::time_point now) {
  impl_->Push(frame, audio, now);
}
void RealtimeFrameScheduler::Reset() { impl_->Reset(); }
void RealtimeFrameScheduler::Finish() { impl_->Finish(); }
std::optional<RealtimeFrameScheduler::OutputFrame>
RealtimeFrameScheduler::TakeReady(Clock::time_point now) {
  return impl_->TakeReady(now);
}
std::optional<RealtimeFrameScheduler::Clock::time_point>
RealtimeFrameScheduler::deadline() const {
  return impl_->deadline();
}
bool RealtimeFrameScheduler::finished() const { return impl_->finished(); }
bool RealtimeFrameScheduler::standby() const { return impl_->standby(); }
std::size_t RealtimeFrameScheduler::queue_depth() const {
  return impl_->queue_depth();
}

}  // namespace mw::streamer::synchronizer::internal

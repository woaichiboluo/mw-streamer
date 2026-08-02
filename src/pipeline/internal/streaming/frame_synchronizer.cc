#include "mw/pipeline/internal/streaming/frame_synchronizer.h"

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"

namespace mw::streamer::pipeline::internal::streaming {
namespace {

constexpr AVRational kMicroseconds{1, AV_TIME_BASE};

std::int64_t AudioFrameDurationUs(const ffmpeg::Frame& frame) {
  return av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate},
                      kMicroseconds);
}

}  // namespace

FrameSynchronizer::FrameSynchronizer(
    bool has_audio, bool has_video, AVRational video_frame_rate,
    std::chrono::milliseconds max_track_wait, bool standby_enabled,
    std::string standby_image_path,
    const ffmpeg::HardwareContext* hardware_context)
    : has_audio_(has_audio),
      has_video_(has_video),
      video_frame_rate_(video_frame_rate),
      max_track_wait_(max_track_wait),
      standby_enabled_(standby_enabled),
      hardware_context_(hardware_context) {
  if (!has_audio_ && !has_video_) {
    throw std::invalid_argument("FrameSynchronizer至少需要一路音频或视频");
  }
  if (max_track_wait_ < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("FrameSynchronizer最大等待时间不能为负数");
  }
  if (has_video_ &&
      (video_frame_rate_.num <= 0 || video_frame_rate_.den <= 0)) {
    throw std::invalid_argument("FrameSynchronizer视频帧率必须为正数");
  }
  if (standby_enabled_ && has_video_) {
    standby_video_frame_.emplace(std::move(standby_image_path));
  }
}

void FrameSynchronizer::PushAudio(ffmpeg::Frame frame) {
  if (!has_audio_) {
    throw std::logic_error("FrameSynchronizer没有音频轨道");
  }
  ValidateFrame(frame, AVMEDIA_TYPE_AUDIO);
  if (!audio_.initialized) {
    audio_.time_base = frame->time_base;
    audio_duration_ = std::max<std::int64_t>(
        av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate},
                     frame->time_base),
        1);
    audio_.initialized = true;
    PrepareSilenceFrame(frame);
  } else if (av_cmp_q(audio_.time_base, frame->time_base) != 0) {
    throw std::invalid_argument("FrameSynchronizer不支持改变音频time_base");
  }
  Push(audio_frames_, audio_, std::move(frame), AVMEDIA_TYPE_AUDIO);
}

void FrameSynchronizer::PushVideo(ffmpeg::Frame frame) {
  if (!has_video_) {
    throw std::logic_error("FrameSynchronizer没有视频轨道");
  }
  ValidateFrame(frame, AVMEDIA_TYPE_VIDEO);
  if (!video_.initialized) {
    video_.time_base = frame->time_base;
    video_duration_ = std::max<std::int64_t>(
        av_rescale_q(1, av_inv_q(video_frame_rate_), frame->time_base), 1);
    video_.initialized = true;
    if (standby_video_frame_) {
      standby_video_frame_->Prepare(frame, hardware_context_);
    }
  } else if (av_cmp_q(video_.time_base, frame->time_base) != 0) {
    throw std::invalid_argument("FrameSynchronizer不支持改变视频time_base");
  }
  Push(video_frames_, video_, std::move(frame), AVMEDIA_TYPE_VIDEO);
}

void FrameSynchronizer::Interrupt(AVMediaType media_type) {
  MarkTrack(media_type, false);
}

void FrameSynchronizer::Finish(AVMediaType media_type) {
  MarkTrack(media_type, true);
}

std::optional<FrameSynchronizer::OutputFrame> FrameSynchronizer::TakeReady(
    Clock::time_point now) {
  deadline_.reset();

  if (!started_) {
    if (has_audio_ && audio_.finished && audio_frames_.empty()) {
      throw std::runtime_error("音频轨道结束前没有产生可同步帧");
    }
    if (has_video_ && video_.finished && video_frames_.empty()) {
      throw std::runtime_error("视频轨道结束前没有产生可同步帧");
    }
    if (transition_) {
      if (!ReadyToResume()) {
        return std::nullopt;
      }
      transition_ = false;
      audio_.interrupted = false;
      video_.interrupted = false;
    } else if (!ReadyToStart()) {
      return std::nullopt;
    }
    StartRealOutput(false, now);
  }

  if (transition_) {
    if (!AllTracksInterrupted()) {
      return std::nullopt;
    }
    if (ReadyToResume()) {
      StartRealOutput(true, now);
    } else if (!standby_) {
      if (!standby_enabled_) {
        return std::nullopt;
      }
      StartStandby(now);
    }
  }

  if (standby_) {
    return TakeStandbyFrame(now);
  }
  return TakeRealFrame(now);
}

std::optional<FrameSynchronizer::Clock::time_point>
FrameSynchronizer::deadline() const noexcept {
  return deadline_;
}

bool FrameSynchronizer::finished() const noexcept {
  return started_ && AllTracksFinished() && audio_frames_.empty() &&
         video_frames_.empty() && !transition_ && !standby_;
}

void FrameSynchronizer::ValidateFrame(const ffmpeg::Frame& frame,
                                      AVMediaType media_type) {
  if (!frame.get() || frame->pts == AV_NOPTS_VALUE ||
      frame->time_base.num <= 0 || frame->time_base.den <= 0) {
    throw std::invalid_argument("FrameSynchronizer收到无效PTS或time_base");
  }
  if (media_type == AVMEDIA_TYPE_AUDIO) {
    if (frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
        av_channel_layout_check(&frame->ch_layout) != 1) {
      throw std::invalid_argument("FrameSynchronizer收到无效音频帧");
    }
    return;
  }
  if (media_type != AVMEDIA_TYPE_VIDEO || frame->width <= 0 ||
      frame->height <= 0 || frame->format == AV_PIX_FMT_NONE) {
    throw std::invalid_argument("FrameSynchronizer收到无效视频帧");
  }
}

std::int64_t FrameSynchronizer::SourcePtsUs(const ffmpeg::Frame& frame) {
  return av_rescale_q(frame->pts, frame->time_base, kMicroseconds);
}

std::int64_t FrameSynchronizer::TimeUs(const TrackState& track) {
  if (!track.initialized) {
    return 0;
  }
  return av_rescale_q(track.next_pts, track.time_base, kMicroseconds);
}

void FrameSynchronizer::Push(std::deque<TimedFrame>& queue, TrackState& track,
                             ffmpeg::Frame frame, AVMediaType media_type) {
  const auto source_pts_us = SourcePtsUs(frame);
  if (track.last_source_pts_us != AV_NOPTS_VALUE &&
      source_pts_us < track.last_source_pts_us) {
    throw std::invalid_argument(fmt::format(
        "FrameSynchronizer{}PTS不是单调递增: previous_us={}, current_us={}, "
        "current_pts={}, best_effort_timestamp={}, pkt_dts={}, "
        "time_base={}/{}",
        media_type == AVMEDIA_TYPE_AUDIO ? "音频" : "视频",
        track.last_source_pts_us, source_pts_us, frame->pts,
        frame->best_effort_timestamp, frame->pkt_dts, frame->time_base.num,
        frame->time_base.den));
  }
  track.last_source_pts_us = source_pts_us;
  queue.push_back({std::move(frame), source_pts_us, Clock::now()});
}

void FrameSynchronizer::MarkTrack(AVMediaType media_type, bool final_end) {
  TrackState* track = nullptr;
  if (media_type == AVMEDIA_TYPE_AUDIO && has_audio_) {
    track = &audio_;
  } else if (media_type == AVMEDIA_TYPE_VIDEO && has_video_) {
    track = &video_;
  } else {
    throw std::invalid_argument("FrameSynchronizer收到未知轨道边界");
  }

  if (final_end) {
    track->finished = true;
    return;
  }
  if (!transition_) {
    audio_frames_.clear();
    video_frames_.clear();
    audio_.last_source_pts_us = AV_NOPTS_VALUE;
    video_.last_source_pts_us = AV_NOPTS_VALUE;
    transition_ = true;
    deadline_.reset();
  }
  if (media_type == AVMEDIA_TYPE_AUDIO) {
    audio_frames_.clear();
    audio_.last_source_pts_us = AV_NOPTS_VALUE;
  } else {
    video_frames_.clear();
    video_.last_source_pts_us = AV_NOPTS_VALUE;
  }
  track->interrupted = true;
}

bool FrameSynchronizer::AllTracksInterrupted() const noexcept {
  return (!has_audio_ || audio_.interrupted) &&
         (!has_video_ || video_.interrupted);
}

bool FrameSynchronizer::AllTracksFinished() const noexcept {
  return (!has_audio_ || audio_.finished) && (!has_video_ || video_.finished);
}

bool FrameSynchronizer::ReadyToStart() {
  if ((has_audio_ && audio_frames_.empty()) ||
      (has_video_ && video_frames_.empty()) || transition_) {
    return false;
  }
  if (!has_audio_ || !has_video_) {
    return true;
  }
  return AlignFirstFrames();
}

bool FrameSynchronizer::ReadyToResume() {
  if (!AllTracksInterrupted() || (has_audio_ && audio_frames_.empty()) ||
      (has_video_ && video_frames_.empty())) {
    return false;
  }
  if (!has_audio_ || !has_video_) {
    return true;
  }
  return AlignFirstFrames();
}

bool FrameSynchronizer::AlignFirstFrames() {
  const auto video_duration_us =
      av_rescale_q(video_duration_, video_.time_base, kMicroseconds);
  while (!audio_frames_.empty() && !video_frames_.empty()) {
    const auto audio_start = audio_frames_.front().source_pts_us;
    const auto audio_end =
        audio_start + AudioFrameDurationUs(audio_frames_.front().frame);
    const auto video_start = video_frames_.front().source_pts_us;
    const auto video_end = video_start + video_duration_us;
    if (audio_end <= video_start) {
      audio_frames_.pop_front();
      continue;
    }
    if (video_end <= audio_start) {
      video_frames_.pop_front();
      continue;
    }
    return true;
  }
  return false;
}

void FrameSynchronizer::StartRealOutput(bool resumed, Clock::time_point now) {
  if (has_audio_ && has_video_) {
    source_origin_us_ = std::max(audio_frames_.front().source_pts_us,
                                 video_frames_.front().source_pts_us);
  } else {
    source_origin_us_ = has_video_ ? video_frames_.front().source_pts_us
                                   : audio_frames_.front().source_pts_us;
  }
  output_anchor_us_ =
      resumed ? (has_video_ ? TimeUs(video_) : TimeUs(audio_)) : 0;
  real_started_at_ = now;
  real_anchor_us_ = output_anchor_us_;
  audio_.interrupted = false;
  video_.interrupted = false;
  transition_ = false;
  standby_ = false;
  started_ = true;
  force_next_real_video_ = has_video_;
  deadline_.reset();
}

void FrameSynchronizer::StartStandby(Clock::time_point now) {
  if (has_audio_ && !silence_frame_) {
    throw std::logic_error("FrameSynchronizer缺少备播静音帧");
  }
  if (has_video_ &&
      (!standby_video_frame_ || !standby_video_frame_->prepared())) {
    throw std::logic_error("FrameSynchronizer缺少备播视频帧");
  }
  standby_anchor_us_ = has_audio_ ? TimeUs(audio_) : TimeUs(video_);
  if (has_video_) {
    standby_anchor_us_ = std::min(standby_anchor_us_, TimeUs(video_));
  }
  standby_started_at_ = now;
  standby_ = true;
  force_next_real_video_ = has_video_;
}

void FrameSynchronizer::PrepareSilenceFrame(const ffmpeg::Frame& prototype) {
  ffmpeg::Frame silence;
  silence->format = prototype->format;
  silence->sample_rate = prototype->sample_rate;
  silence->time_base = prototype->time_base;
  silence->nb_samples = prototype->nb_samples;
  ffmpeg::ThrowIfError(
      av_channel_layout_copy(&silence->ch_layout, &prototype->ch_layout),
      "复制同步静音帧声道布局");
  ffmpeg::ThrowIfError(av_frame_get_buffer(silence.get(), 0), "分配同步静音帧");
  ffmpeg::ThrowIfError(
      av_samples_set_silence(silence->extended_data, 0, silence->nb_samples,
                             silence->ch_layout.nb_channels,
                             static_cast<AVSampleFormat>(silence->format)),
      "填充同步静音帧");
  silence.CopyPropertiesFrom(prototype);
  silence->pts = AV_NOPTS_VALUE;
  silence->duration = 0;
  silence_frame_.emplace(std::move(silence));
}

std::optional<FrameSynchronizer::OutputFrame>
FrameSynchronizer::TakeStartingFrame() {
  if (force_next_real_video_ && !video_frames_.empty()) {
    force_next_real_video_ = false;
    return TakeRealVideo(true);
  }
  return std::nullopt;
}

std::optional<FrameSynchronizer::OutputFrame> FrameSynchronizer::TakeRealFrame(
    Clock::time_point now) {
  if (auto first_video = TakeStartingFrame()) {
    return first_video;
  }

  const auto audio_duration_us =
      has_audio_
          ? av_rescale_q(audio_duration_, audio_.time_base, kMicroseconds)
          : 0;
  const auto video_duration_us =
      has_video_
          ? av_rescale_q(video_duration_, video_.time_base, kMicroseconds)
          : 0;
  const bool dropped_audio =
      has_audio_ && DropLateFrames(audio_frames_, audio_, audio_duration_us);
  const bool dropped_video =
      has_video_ && DropLateFrames(video_frames_, video_, video_duration_us);
  if (finished()) {
    return std::nullopt;
  }

  if (!has_audio_) {
    if (video_frames_.empty()) {
      if (dropped_video && last_video_frame_) {
        return TakeRepeatedVideo(false);
      }
      return std::nullopt;
    }
    const auto desired = DesiredTimeUs(video_frames_.front());
    const auto next = TimeUs(video_);
    if (last_video_frame_ && next + video_duration_us / 2 < desired) {
      return TakeRepeatedVideo(false);
    }
    if (dropped_video && last_video_frame_) {
      return TakeRepeatedVideo(false);
    }
    return TakeRealVideo(false);
  }
  if (!has_video_) {
    if (audio_frames_.empty()) {
      if (dropped_audio) {
        return TakeAudio(true);
      }
      return std::nullopt;
    }
    const auto desired = DesiredTimeUs(audio_frames_.front());
    if (TimeUs(audio_) + audio_duration_us / 2 < desired) {
      return TakeAudio(true);
    }
    return TakeAudio(false);
  }

  const bool audio_first = AudioDueBeforeVideo();
  auto& due_frames = audio_first ? audio_frames_ : video_frames_;
  const auto& other_frames = audio_first ? video_frames_ : audio_frames_;
  TrackState& due_track = audio_first ? audio_ : video_;
  const auto due_duration =
      audio_first
          ? av_rescale_q(audio_duration_, audio_.time_base, kMicroseconds)
          : av_rescale_q(video_duration_, video_.time_base, kMicroseconds);

  if (!due_frames.empty()) {
    if (audio_first) {
      const auto desired = DesiredTimeUs(due_frames.front());
      if (TimeUs(due_track) + due_duration / 2 < desired) {
        return TakeAudio(true);
      }
      return TakeAudio(false);
    }
    const auto desired = DesiredTimeUs(due_frames.front());
    if (TimeUs(due_track) + due_duration / 2 < desired) {
      return TakeRepeatedVideo(false);
    }
    return TakeRealVideo(false);
  }

  const bool dropped_due = audio_first ? dropped_audio : dropped_video;
  if (dropped_due) {
    return audio_first ? TakeAudio(true) : TakeRepeatedVideo(false);
  }

  if (other_frames.empty()) {
    return std::nullopt;
  }
  const auto wait_deadline = MissingTrackDeadline();
  if (wait_deadline && now < *wait_deadline) {
    deadline_ = wait_deadline;
    return std::nullopt;
  }
  return audio_first ? TakeAudio(true) : TakeRepeatedVideo(false);
}

std::optional<FrameSynchronizer::OutputFrame>
FrameSynchronizer::TakeStandbyFrame(Clock::time_point now) {
  if (ReadyToResume()) {
    StartRealOutput(true, now);
    return TakeRealFrame(now);
  }

  const bool audio_first = has_audio_ && (!has_video_ || AudioDueBeforeVideo());
  const auto next_us = audio_first ? TimeUs(audio_) : TimeUs(video_);
  const auto elapsed_us =
      std::max<std::int64_t>(next_us - standby_anchor_us_, 0);
  const auto due_at =
      standby_started_at_ + std::chrono::microseconds(elapsed_us);
  if (now < due_at) {
    deadline_ = due_at;
    return std::nullopt;
  }
  if (audio_first) {
    return TakeAudio(true);
  }
  const bool force = std::exchange(force_next_real_video_, false);
  return TakeStandbyVideo(force);
}

FrameSynchronizer::OutputFrame FrameSynchronizer::TakeAudio(bool silence) {
  ffmpeg::Frame frame =
      silence ? silence_frame_->Ref() : std::move(audio_frames_.front().frame);
  if (!silence) {
    audio_frames_.pop_front();
  }
  const auto duration = std::max<std::int64_t>(
      av_rescale_q(frame->nb_samples, AVRational{1, frame->sample_rate},
                   audio_.time_base),
      1);
  frame->time_base = audio_.time_base;
  frame->pts = audio_.next_pts;
  frame->duration = duration;
  audio_.next_pts += duration;
  return {AVMEDIA_TYPE_AUDIO, std::move(frame), false};
}

FrameSynchronizer::OutputFrame FrameSynchronizer::TakeRealVideo(
    bool force_key_frame) {
  auto frame = std::move(video_frames_.front().frame);
  video_frames_.pop_front();
  frame->time_base = video_.time_base;
  frame->pts = video_.next_pts;
  frame->duration = video_duration_;
  video_.next_pts += video_duration_;
  last_video_frame_.emplace(frame.Ref());
  return {AVMEDIA_TYPE_VIDEO, std::move(frame), force_key_frame};
}

FrameSynchronizer::OutputFrame FrameSynchronizer::TakeRepeatedVideo(
    bool force_key_frame) {
  auto frame = last_video_frame_->Ref();
  frame->time_base = video_.time_base;
  frame->pts = video_.next_pts;
  frame->duration = video_duration_;
  video_.next_pts += video_duration_;
  return {AVMEDIA_TYPE_VIDEO, std::move(frame), force_key_frame};
}

FrameSynchronizer::OutputFrame FrameSynchronizer::TakeStandbyVideo(
    bool force_key_frame) {
  auto frame = standby_video_frame_->Ref();
  frame->time_base = video_.time_base;
  frame->pts = video_.next_pts;
  frame->duration = video_duration_;
  video_.next_pts += video_duration_;
  return {AVMEDIA_TYPE_VIDEO, std::move(frame), force_key_frame};
}

bool FrameSynchronizer::DropLateFrames(std::deque<TimedFrame>& queue,
                                       TrackState& track,
                                       std::int64_t duration_us) {
  bool dropped = false;
  while (!queue.empty() &&
         DesiredTimeUs(queue.front()) + duration_us / 2 < TimeUs(track)) {
    queue.pop_front();
    dropped = true;
  }
  return dropped;
}

std::int64_t FrameSynchronizer::DesiredTimeUs(const TimedFrame& frame) const {
  return frame.source_pts_us - source_origin_us_ + output_anchor_us_;
}

bool FrameSynchronizer::AudioDueBeforeVideo() const noexcept {
  if (!has_audio_) {
    return false;
  }
  if (!has_video_) {
    return true;
  }
  return TimeUs(audio_) < TimeUs(video_);
}

std::optional<FrameSynchronizer::Clock::time_point>
FrameSynchronizer::MissingTrackDeadline() const noexcept {
  const bool audio_missing = AudioDueBeforeVideo();
  const auto& available_frames = audio_missing ? video_frames_ : audio_frames_;
  const TrackState& missing_track = audio_missing ? audio_ : video_;
  if (missing_track.finished || available_frames.empty()) {
    return std::nullopt;
  }
  const auto playback_due =
      real_started_at_ + std::chrono::microseconds(std::max<std::int64_t>(
                             TimeUs(missing_track) - real_anchor_us_, 0));
  return std::max(available_frames.front().received_at, playback_due) +
         max_track_wait_;
}

}  // namespace mw::streamer::pipeline::internal::streaming

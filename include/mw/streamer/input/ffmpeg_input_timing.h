#ifndef MW_STREAMER_INPUT_FFMPEG_INPUT_TIMING_H_
#define MW_STREAMER_INPUT_FFMPEG_INPUT_TIMING_H_

#include <chrono>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

namespace mw::streamer::internal {

// Shared by input timestamp publication and output scheduling inside the
// library.
std::chrono::steady_clock::time_point SystemTimeBase() noexcept;

// Keep estimates in nanoseconds; round only when publishing AVFrame fields.
class FrameTiming final {
 public:
  FrameTiming(AVRational time_base, AVRational frame_rate, bool audio,
              std::int64_t last_duration_ns = 0)
      : time_base_(time_base),
        frame_rate_(frame_rate),
        audio_(audio),
        last_duration_ns_(last_duration_ns) {}

  void Update(AVFrame& frame) {
    const auto last_pts = pts_ns_;
    const bool known_pts = frame.best_effort_timestamp != AV_NOPTS_VALUE;
    pts_ns_ = known_pts ? av_rescale_q(frame.best_effort_timestamp, time_base_,
                                       kNanoseconds)
                        : next_pts_ns_;
    const auto duration =
        frame.duration ? av_rescale_q(frame.duration, time_base_, kNanoseconds)
                       : EstimateDuration(frame, last_pts);
    last_duration_ns_ = duration;
    next_pts_ns_ = pts_ns_ + duration;
    frame.pts = known_pts ? frame.best_effort_timestamp
                          : av_rescale_q(pts_ns_, kNanoseconds, time_base_);
    frame.duration = av_rescale_q(duration, kNanoseconds, time_base_);
  }

  std::int64_t pts_ns() const noexcept { return pts_ns_; }
  std::int64_t duration_ns() const noexcept { return last_duration_ns_; }
  std::int64_t next_pts_ns() const noexcept { return next_pts_ns_; }

  void Flush() noexcept {
    pts_ns_ = 0;
    next_pts_ns_ = 0;
    // Retain the last duration as the post-seek estimation fallback.
  }

 private:
  std::int64_t EstimateDuration(const AVFrame& frame,
                                std::int64_t last_pts) const {
    if (audio_) {
      return frame.sample_rate > 0
                 ? av_rescale_q(frame.nb_samples,
                                AVRational{1, frame.sample_rate}, kNanoseconds)
                 : 0;
    }
    if (last_pts) {
      return pts_ns_ - last_pts;
    }
    if (last_duration_ns_) {
      return last_duration_ns_;
    }
    // FFmpeg 8 no longer uses AVCodecContext::time_base for decoding. Use
    // the stream's frame rate for the final nominal-duration estimate.
    return frame_rate_.num > 0 && frame_rate_.den > 0
               ? av_rescale_q(1, av_inv_q(frame_rate_), kNanoseconds)
               : 0;
  }

  static constexpr AVRational kNanoseconds{1, 1000000000};
  AVRational time_base_;
  AVRational frame_rate_;
  bool audio_;
  std::int64_t pts_ns_ = 0;
  std::int64_t next_pts_ns_ = 0;
  std::int64_t last_duration_ns_ = 0;
};

// Both tracks share a scheduling clock and output timestamp mapping.
// Discontinuities suppress the scheduling increment without changing the
// output mapping or concealing accumulated lateness.
class PlaybackClock final {
 public:
  using Clock = std::chrono::steady_clock;

  PlaybackClock(std::int64_t first_pts, Clock::time_point started,
                Clock::time_point epoch = {}, AVRational speed = {1, 1})
      : pts_ns_(first_pts),
        deadline_(started),
        start_ts_ns_(first_pts),
        play_sys_ts_ns_(ToNs(started)),
        base_sys_ts_ns_(ToNs(epoch)),
        speed_(speed) {}

  // Keep prediction and discontinuity checks in the original media time base.
  std::int64_t ScaleTime(std::int64_t media_ns) const noexcept {
    return av_rescale_q(media_ns, av_inv_q(speed_), AVRational{1, 1});
  }

  AVRational speed() const noexcept { return speed_; }

  std::int64_t Timestamp(std::int64_t media_pts) const noexcept {
    return ScaleTime(base_ts_ns_ + media_pts - start_ts_ns_) + play_sys_ts_ns_ -
           base_sys_ts_ns_;
  }

  // Preparation records the output timestamp anchor; delivery initializes
  // the scheduling deadline. Keep these two wall-clock anchors separate.
  void StartDelivery(Clock::time_point now) noexcept { deadline_ = now; }

  Clock::time_point LoopDeadline(std::int64_t end_pts) const noexcept {
    return deadline_ + std::chrono::nanoseconds(
                           ScaleTime(scheduled_media_ns_ + end_pts - pts_ns_) -
                           ScaleTime(scheduled_media_ns_));
  }

  // Accumulate absolute max(next_pts), even for a nonzero media origin.
  // Reopening must not establish a fresh wall clock or hide decoding delays.
  void EndLoop(std::int64_t end_pts) noexcept {
    deadline_ = LoopDeadline(end_pts);
    scheduled_media_ns_ += end_pts - pts_ns_;
    base_ts_ns_ += end_pts;
  }

  void BeginLoop(std::int64_t first_pts) noexcept {
    start_ts_ns_ = pts_ns_ = first_pts;
  }

  void Advance(std::int64_t pts) noexcept {
    auto delta = pts - pts_ns_;
    if (delta < 0 || delta > 3000000000LL) {
      delta = 0;
    }
    // Scale accumulated media time rather than each delta, avoiding drift
    // when a frame interval cannot be represented exactly at this speed.
    deadline_ +=
        std::chrono::nanoseconds(ScaleTime(scheduled_media_ns_ + delta) -
                                 ScaleTime(scheduled_media_ns_));
    scheduled_media_ns_ += delta;
    pts_ns_ = pts;
  }

  // Suppress the first post-seek scheduling delta while retaining
  // the existing wall deadline. Subsequent frames advance normally.
  void Seek(std::int64_t pts) noexcept { pts_ns_ = pts; }

  bool CanDeliver(std::int64_t pts) const noexcept {
    // Release a leading track with an anomalous >2s timestamp gap.
    return pts <= pts_ns_ || pts - pts_ns_ > 2000000000LL;
  }

  Clock::time_point deadline() const noexcept { return deadline_; }

 private:
  static std::int64_t ToNs(Clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               value.time_since_epoch())
        .count();
  }

  std::int64_t pts_ns_;
  Clock::time_point deadline_;
  std::int64_t base_ts_ns_ = 0;
  std::int64_t start_ts_ns_;
  std::int64_t play_sys_ts_ns_;
  std::int64_t base_sys_ts_ns_;
  AVRational speed_;
  std::int64_t scheduled_media_ns_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INPUT_FFMPEG_INPUT_TIMING_H_

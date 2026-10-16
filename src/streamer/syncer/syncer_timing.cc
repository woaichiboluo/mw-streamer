#include "mw/streamer/syncer/syncer_timing.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "mw/log.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kMicroseconds{1, 1000000};
constexpr std::int64_t kPollUs = 22000;
constexpr std::int64_t kNoWake = std::numeric_limits<std::int64_t>::max();

const char* TrackName(bool video) { return video ? "video" : "audio"; }

ffmpeg::Frame BlackFrame(const AvEncoderConfig& config) {
  ffmpeg::Frame frame;
  frame->width = config.width;
  frame->height = config.height;
  frame->format = config.video_pixel_format;
  const auto* descriptor = av_pix_fmt_desc_get(config.video_pixel_format);
  if (!descriptor || config.width <= 0 || config.height <= 0) {
    throw std::invalid_argument("Syncer 默认黑帧需要有效的视频目标格式和尺寸");
  }
  const auto format = config.video_pixel_format;
  const bool full_range =
      (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0 ||
      format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
      format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P ||
      format == AV_PIX_FMT_YUVJ411P;
  frame->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 Syncer CPU 黑帧");
  const ptrdiff_t strides[]{frame->linesize[0], frame->linesize[1],
                            frame->linesize[2], frame->linesize[3]};
  ffmpeg::FfmpegException::throwIfError(
      av_image_fill_black(frame->data, strides, format, frame->color_range,
                          config.width, config.height),
      "填充 Syncer CPU 黑帧");
  return frame;
}

long double DiagnosticDifferenceUs(std::int64_t a, std::int64_t b,
                                   AVRational time_base) {
  return (static_cast<long double>(a) - b) * time_base.num * 1000000 /
         time_base.den;
}

std::int64_t Add(std::int64_t a, std::int64_t b) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) {
    throw std::overflow_error("Syncer 时间计算溢出");
  }
  return a + b;
}

std::int64_t Subtract(std::int64_t a, std::int64_t b) {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("Syncer 时间计算溢出");
  }
  return Add(a, -b);
}

std::int64_t Rescale(std::int64_t value, AVRational from, AVRational to) {
  const auto result = av_rescale_q(value, from, to);
  if (result == AV_NOPTS_VALUE || result == kNoWake) {
    throw std::overflow_error("Syncer 时间戳换算溢出");
  }
  return result;
}

bool Apart(std::int64_t a, std::int64_t b, std::int64_t tolerance) {
  // A difference equal to the configured tolerance is a discontinuity.
  const auto difference = Subtract(a, b);
  return difference >= tolerance || difference <= -tolerance;
}

// Hold the last advancing real timestamp for two seconds, then extrapolate
// from elapsed steady-clock time. Backup output does not refresh the anchor.
class ContinuityClock final {
 public:
  ContinuityClock(std::int64_t start, const void* log_context)
      : start_(start), log_context_(log_context) {}
  std::int64_t Now(std::int64_t now) {
    if (!pts_) Update(start_, now);
    const auto elapsed = Subtract(now, wall_);
    if (elapsed >= 2000000 && !extrapolating_) {
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 共享媒体时钟转为墙钟外推: anchor_pts_us={} "
                  "anchor_wall_us={} now_us={} elapsed_us={}",
                  log_context_, *pts_, wall_, now, elapsed);
      extrapolating_ = true;
    }
    return elapsed < 2000000 ? *pts_ : Add(*pts_, elapsed);
  }
  void Update(std::int64_t pts, std::int64_t now) {
    if (!pts_ || pts > *pts_) {
      if (extrapolating_) {
        MW_LOG_INFO("streamer",
                    "Syncer[{}] 共享媒体时钟恢复媒体锚定: pts_us={} now_us={}",
                    log_context_, pts, now);
        extrapolating_ = false;
      }
      pts_ = pts;
      wall_ = now;
    }
  }
  std::optional<std::int64_t> shift;

 private:
  const std::int64_t start_;
  const void* log_context_;
  bool extrapolating_ = false;
  std::optional<std::int64_t> pts_;
  std::int64_t wall_ = 0;
};

struct Track {
  AVRational time_base{0, 1};
  std::optional<std::int64_t> next;
  std::optional<std::int64_t> shift;
  std::optional<ffmpeg::Frame> last_real;
  std::int64_t last_real_pts = 0;
  bool last_success = true;
  bool has_output = false;
  std::int64_t wait_until = 0;
  bool try_without_filling = false;
  // A forward jump or timeout can produce at most five fillers per iteration.
  std::optional<std::int64_t> fill_until;
  bool forward_fill = false;
  int filled = 0;
  std::int64_t filled_us = 0;
  std::optional<std::int64_t> pending_real_pts;
  std::int64_t suspended_until = 0;
  // Diagnostics only: these fields never participate in scheduling decisions.
  bool timed_out = false;
  const char* output_source = nullptr;
};

}  // namespace

class SyncerTiming::Impl final {
 public:
  Impl(const SyncerConfig& config, const void* log_context)
      : config_(config),
        log_context_(log_context),
        clock_(config_.start_ts.count(), log_context) {
    const bool video = config_.encoder.video_pixel_format != AV_PIX_FMT_NONE;
    video_.time_base = video ? av_inv_q(config_.encoder.fps) : kMicroseconds;
    if (video && !config_.backup_video) {
      config_.backup_video = BlackFrame(config_.encoder);
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 创建默认 CPU 黑帧: width={} height={} format={}",
                  log_context_, config_.encoder.width, config_.encoder.height,
                  static_cast<int>(config_.encoder.video_pixel_format));
    }
    audio_.time_base = config_.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE
                           ? AVRational{1, config_.encoder.audio_sample_rate}
                           : kMicroseconds;
    for (auto* track : {&video_, &audio_}) {
      track->wait_until = config_.timeout.count();
      if (!config_.forward_start_shift) {
        track->next = FromUs(config_.start_ts.count(), *track);
      }
    }
    if (config_.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      if (av_channel_layout_copy(&audio_layout_->ch_layout,
                                 &config_.encoder.audio_channel_layout) < 0) {
        throw std::runtime_error("Syncer 复制音频布局失败");
      }
      config_.encoder.audio_channel_layout = audio_layout_->ch_layout;
    }
  }

  SyncerStep Step(bool video, const AvDecoderReadResult* head,
                  std::int64_t now) {
    auto& track = video ? video_ : audio_;
    if (track.fill_until) return Fill(track, video, head, now);
    if (track.pending_real_pts) {
      if (now < track.suspended_until) {
        return {false, {}, track.suspended_until};
      }
      track.pending_real_pts.reset();
      // The peer may have corrected the shared shift during this suspension.
      return Correct(track, video, *head, now);
    }
    if (head) {
      if (head->status == AvDecoderReadStatus::kEof) {
        // Like a live sentinel, consume EOF and keep both clocks running.
        MW_LOG_INFO("streamer",
                    "Syncer[{}] 消费输入 EOF，继续实时输出: track={} "
                    "end_pts={} time_base={}/{} now_us={}",
                    log_context_, TrackName(video), head->end_pts,
                    head->time_base.num, head->time_base.den, now);
        return {true, {}, now};
      }
      return Correct(track, video, *head, now);
    }
    if (now < track.wait_until) {
      return {false, {}, track.wait_until};
    }
    if (!track.timed_out) {
      track.timed_out = true;
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 输入超时 track={} "
                  "now_us={} deadline_us={} has_head={}",
                  log_context_, TrackName(video), now, track.wait_until,
                  head != nullptr);
    }
    const auto rtc = clock_.Now(now);
    if (!track.next) track.next = FromUs(config_.start_ts.count(), track);
    const bool fill =
        !track.last_success ||
        Subtract(rtc, ToUs(*track.next, track)) > config_.timeout.count();
    track.last_success = false;
    track.wait_until = Add(now, kPollUs);
    const auto target = FromUs(rtc, track);
    if (fill && *track.next < target) {
      BeginFill(track, video, target, false, now);
      return Fill(track, video, head, now);
    }
    return {false, {}, track.wait_until};
  }

 private:
  static std::int64_t ToUs(std::int64_t value, const Track& track) {
    return Rescale(value, track.time_base, kMicroseconds);
  }
  static std::int64_t FromUs(std::int64_t value, const Track& track) {
    return Rescale(value, kMicroseconds, track.time_base);
  }

  SyncerStep Correct(Track& track, bool video, const AvDecoderReadResult& input,
                     std::int64_t now) {
    if (video && av_cmp_q(track.time_base, input.time_base) != 0) {
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 视频时间基变更: old_tb={}/{} "
                  "new_tb={}/{} input_pts={} now_us={}",
                  log_context_, track.time_base.num, track.time_base.den,
                  input.time_base.num, input.time_base.den,
                  input.frame->get()->pts, now);
      ChangeTimeBase(track, input.time_base);
    }
    const auto original =
        Rescale(input.frame->get()->pts, input.time_base, track.time_base);
    std::int64_t pts;
    if (!clock_.shift) {
      pts = FromUs(clock_.Now(now), track);
      // A bootstrap backup can extend beyond the held shared clock. The first
      // real frame must start after it, rather than overlap its last interval.
      if (track.has_output) pts = std::max(pts, *track.next);
      track.shift = Subtract(pts, original);
      clock_.shift = ToUs(*track.shift, track);
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 输出时间轴首次锚定 track={} "
                  "input_pts={} input_tb={}/{} mapped_pts={} output_tb={}/{} "
                  "local_shift={} shared_shift_us={} now_us={}",
                  log_context_, TrackName(video), input.frame->get()->pts,
                  input.time_base.num, input.time_base.den, pts,
                  track.time_base.num, track.time_base.den, *track.shift,
                  *clock_.shift, now);
    } else {
      const bool desync =
          track.shift && Apart(ToUs(*track.shift, track), *clock_.shift,
                               Add(config_.max_streams_diff.count(), 1));
      if (!track.shift || desync) {
        // Adopt the shared shift only when the difference exceeds the
        // configured tolerance; timestamp discontinuity includes equality.
        const bool had_shift = track.shift.has_value();
        const auto old_shift = track.shift.value_or(0);
        track.shift = FromUs(*clock_.shift, track);
        MW_LOG_INFO("streamer",
                    "Syncer[{}] 采纳共享时间偏移 track={} "
                    "input_pts={} input_tb={}/{} output_tb={}/{} had_shift={} "
                    "old_local_shift={} new_local_shift={} shared_shift_us={} "
                    "mapped_pts={:.0f} has_next={} next_pts={} now_us={}",
                    log_context_, TrackName(video), input.frame->get()->pts,
                    input.time_base.num, input.time_base.den,
                    track.time_base.num, track.time_base.den, had_shift,
                    old_shift, *track.shift, *clock_.shift,
                    static_cast<long double>(original) + *track.shift,
                    track.next.has_value(), track.next.value_or(0), now);
      }
      pts = Add(original, *track.shift);
      if (track.next && (pts != *track.next &&
                         Apart(ToUs(pts, track), ToUs(*track.next, track),
                               config_.max_streams_diff.count()))) {
        if (pts > *track.next) {
          if (!track.try_without_filling) {
            BeginFill(track, video, pts, true, now);
            return Fill(track, video, &input, now);
          }
          track.try_without_filling = false;
          Adjust(track, video, input, original, pts, "forward_jump", now);
          track.pending_real_pts = *track.next;
          track.suspended_until = Add(now, 1000);
          return {false, {}, track.suspended_until};
        }
        Adjust(track, video, input, original, pts, "backward_jump", now);
        pts = *track.next;
      } else if (desync) {
        // Sentinel also publishes the adopted local shift when this track
        // stayed continuous. Its time base may round the shared shift, and
        // peers must see that representable value to complete synchronization.
        clock_.shift = ToUs(*track.shift, track);
      }
    }
    return Real(track, video, input, pts, now);
  }

  static void ChangeTimeBase(Track& track, AVRational time_base) {
    const auto convert = [&](std::optional<std::int64_t>& value) {
      if (value) value = Rescale(*value, track.time_base, time_base);
    };
    convert(track.next);
    convert(track.shift);
    convert(track.fill_until);
    convert(track.pending_real_pts);
    if (track.last_real) {
      track.last_real_pts =
          Rescale(track.last_real_pts, track.time_base, time_base);
    }
    track.time_base = time_base;
  }

  std::int64_t VideoDuration(const Track& track) const {
    const auto duration =
        Rescale(1, av_inv_q(config_.encoder.fps), track.time_base);
    if (duration <= 0) {
      throw std::invalid_argument("Syncer 视频 time base 无法表示帧周期");
    }
    return duration;
  }

  void Adjust(Track& track, bool video, const AvDecoderReadResult& input,
              std::int64_t original, std::int64_t mapped_pts,
              const char* reason, std::int64_t now) {
    const auto old_local = track.shift.value_or(0);
    const auto old_shared = clock_.shift.value_or(0);
    track.shift = Subtract(*track.next, original);
    clock_.shift = ToUs(*track.shift, track);
    // Diagnostic differences use floating point so logging cannot introduce
    // additional checked timestamp arithmetic or overflow failures.
    MW_LOG_INFO(
        "streamer",
        "Syncer[{}] 修正输出时间偏移 reason={} track={} "
        "input_pts={} input_tb={}/{} mapped_pts={} next_pts={} "
        "output_tb={}/{} diff_us={:.3f} old_local_shift={} new_local_shift={} "
        "old_shared_shift_us={} new_shared_shift_us={} now_us={}",
        log_context_, reason, TrackName(video), input.frame->get()->pts,
        input.time_base.num, input.time_base.den, mapped_pts, *track.next,
        track.time_base.num, track.time_base.den,
        DiagnosticDifferenceUs(mapped_pts, *track.next, track.time_base),
        old_local, *track.shift, old_shared, *clock_.shift, now);
  }

  void BeginFill(Track& track, bool video, std::int64_t target, bool forward,
                 std::int64_t now) {
    track.fill_until = target;
    track.forward_fill = forward;
    track.filled = 0;
    track.filled_us = 0;
    MW_LOG_DEBUG("streamer",
                 "Syncer[{}] 开始填充批次 reason={} track={} "
                 "next_pts={} target_pts={} "
                 "output_tb={}/{} now_us={}",
                 log_context_, forward ? "forward_jump" : "timeout",
                 TrackName(video), *track.next, target, track.time_base.num,
                 track.time_base.den, now);
  }

  void ObserveSource(Track& track, bool video, const char* source,
                     std::int64_t now) const {
    if (track.output_source && std::string_view(track.output_source) == source)
      return;
    MW_LOG_INFO("streamer",
                "Syncer[{}] 输出来源切换 track={} "
                "old_source={} source={} now_us={}",
                log_context_, TrackName(video),
                track.output_source ? track.output_source : "none", source,
                now);
    track.output_source = source;
  }

  ffmpeg::Frame Backup(Track& track, bool video, std::int64_t count,
                       const char*& source) {
    if (video) {
      if (track.last_real &&
          Subtract(ToUs(*track.next, track), ToUs(track.last_real_pts, track)) <
              config_.freeze.count()) {
        source = "freeze";
        return track.last_real->Ref();
      }
      source = "backup";
      return config_.backup_video->Ref();
    }
    source = "silence";
    ffmpeg::Frame frame;
    auto* raw = frame.get();
    raw->format = config_.encoder.audio_sample_format;
    raw->sample_rate = config_.encoder.audio_sample_rate;
    raw->nb_samples = static_cast<int>(std::min<std::int64_t>(1024, count));
    if (av_channel_layout_copy(&raw->ch_layout,
                               &config_.encoder.audio_channel_layout) < 0 ||
        av_frame_get_buffer(raw, 0) < 0 ||
        av_samples_set_silence(raw->extended_data, 0, raw->nb_samples,
                               raw->ch_layout.nb_channels,
                               config_.encoder.audio_sample_format) < 0) {
      throw std::runtime_error("Syncer 创建静音帧失败");
    }
    return frame;
  }

  static void Stamp(ffmpeg::Frame& frame, const Track& track, std::int64_t pts,
                    std::int64_t duration) {
    auto* raw = frame.get();
    raw->pts = pts;
    raw->best_effort_timestamp = pts;
    raw->pkt_dts = AV_NOPTS_VALUE;
    raw->time_base = track.time_base;
    raw->duration = duration;
  }

  SyncerStep Real(Track& track, bool video, const AvDecoderReadResult& input,
                  std::int64_t pts, std::int64_t now) {
    auto frame = input.frame->Ref();
    const auto duration = video ? VideoDuration(track) : frame->nb_samples;
    Stamp(frame, track, pts, duration);
    track.next = Add(pts, duration);
    track.has_output = true;
    if (track.timed_out) {
      MW_LOG_INFO("streamer", "Syncer[{}] 输入超时恢复 track={} now_us={}",
                  log_context_, TrackName(video), now);
      track.timed_out = false;
    }
    ObserveSource(track, video, "input", now);
    MW_LOG_TRACE("streamer",
                 "Syncer[{}] 输出帧 track={} reason=input "
                 "source=input input_pts={} input_tb={}/{} output_pts={} "
                 "output_tb={}/{} next_pts={} duration={} samples={} now_us={}",
                 log_context_, TrackName(video), input.frame->get()->pts,
                 input.time_base.num, input.time_base.den, pts,
                 track.time_base.num, track.time_base.den, *track.next,
                 duration, video ? 0 : frame->nb_samples, now);
    clock_.Update(ToUs(pts, track), now);
    if (video) track.last_real = frame.Ref();
    track.last_real_pts = pts;
    track.last_success = true;
    track.wait_until = Add(now, config_.timeout.count());
    track.pending_real_pts.reset();
    return {true, std::move(frame), now};
  }

  SyncerStep Fill(Track& track, bool video, const AvDecoderReadResult* input,
                  std::int64_t now) {
    const auto pts = *track.next;
    const char* source = nullptr;
    auto frame = Backup(track, video, Subtract(*track.fill_until, pts), source);
    const std::int64_t duration =
        video ? VideoDuration(track) : frame->nb_samples;
    Stamp(frame, track, pts, duration);
    track.next = Add(pts, duration);
    track.has_output = true;
    ++track.filled;
    track.filled_us = Add(track.filled_us, ToUs(duration, track));
    ObserveSource(track, video, source, now);
    MW_LOG_TRACE(
        "streamer",
        "Syncer[{}] 输出填充帧 track={} "
        "reason={} source={} has_input={} input_pts={} "
        "input_tb={}/{} output_pts={} output_tb={}/{} next_pts={} "
        "duration={} samples={} now_us={}",
        log_context_, TrackName(video),
        track.forward_fill ? "forward_jump" : "timeout", source,
        input != nullptr,
        input && input->frame ? input->frame->get()->pts : AV_NOPTS_VALUE,
        input ? input->time_base.num : 0, input ? input->time_base.den : 1, pts,
        track.time_base.num, track.time_base.den, *track.next, duration,
        video ? 0 : frame->nb_samples, now);
    std::int64_t wake = now;
    if (*track.next >= *track.fill_until || track.filled == 5) {
      if (track.forward_fill) {
        // Limit forward-jump fill to five frames and move the pending real
        // frame to the next output PTS. A full batch waits half its duration
        // before retrying to avoid emitting all replacement frames at once.
        if (track.filled == 5) clock_.Update(ToUs(pts, track), now);
        const auto original = Rescale(input->frame->get()->pts,
                                      input->time_base, track.time_base);
        Adjust(track, video, *input, original, *track.fill_until,
               "forward_jump", now);
        track.try_without_filling = true;
        track.pending_real_pts = *track.next;
        track.suspended_until =
            track.filled == 5
                ? Add(now, std::max<std::int64_t>(1000, track.filled_us / 2))
                : now;
        wake = track.suspended_until;
      } else {
        wake = track.wait_until;
      }
      MW_LOG_DEBUG(
          "streamer",
          "Syncer[{}] 结束填充批次 reason={} track={} "
          "count={} duration_us={} "
          "target_pts={} next_pts={} output_tb={}/{} now_us={} "
          "resume_us={}",
          log_context_, track.forward_fill ? "forward_jump" : "timeout",
          TrackName(video), track.filled, track.filled_us, *track.fill_until,
          *track.next, track.time_base.num, track.time_base.den, now, wake);
      track.fill_until.reset();
    }
    return {false, std::move(frame), wake};
  }

  SyncerConfig config_;
  const void* log_context_;
  ffmpeg::Frame audio_layout_;
  ContinuityClock clock_;
  Track video_;
  Track audio_;
};

SyncerTiming::SyncerTiming(const SyncerConfig& config, const void* log_context)
    : impl_(std::make_unique<Impl>(config, log_context ? log_context : this)) {}
SyncerTiming::~SyncerTiming() = default;
SyncerStep SyncerTiming::Step(bool video, const AvDecoderReadResult* head,
                              std::int64_t now_us) {
  return impl_->Step(video, head, now_us);
}

}  // namespace mw::streamer::internal

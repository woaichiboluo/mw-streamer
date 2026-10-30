#include "mw/streamer/syncer/syncer.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "streamer/performance/latency.h"
#include "streamer/syncer/syncer_timing.h"

namespace mw::streamer {
namespace {

constexpr AVRational kMicroseconds{1, 1000000};

bool ValidVideoFrame(const AVFrame& frame) {
  return frame.data[0] && frame.width > 0 && frame.height > 0 &&
         frame.nb_samples == 0 &&
         av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
}

void ValidateConfig(const SyncerConfig& config) {
  const bool video = config.encoder.video_pixel_format != AV_PIX_FMT_NONE;
  const bool audio = config.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE;
  if ((!video && !audio) || config.options.initial_timeout.count() <= 0 ||
      config.options.timeout.count() <= 0 ||
      config.options.freeze.count() < 0 ||
      config.options.max_streams_diff.count() <= 0) {
    throw std::invalid_argument("Syncer 轨道或时间阈值无效");
  }
  if (video) {
    if (config.encoder.fps.num <= 0 || config.encoder.fps.den <= 0 ||
        av_rescale_q(1, av_inv_q(config.encoder.fps), kMicroseconds) <= 0) {
      throw std::invalid_argument("Syncer 视频帧率无效");
    }
    if (config.backup_video) {
      const auto* frame = config.backup_video->get();
      if (!frame || !ValidVideoFrame(*frame)) {
        throw std::invalid_argument("Syncer 备播图不是有效的视频帧");
      }
    } else {
      const auto* descriptor =
          av_pix_fmt_desc_get(config.encoder.video_pixel_format);
      if (config.encoder.width <= 0 || config.encoder.height <= 0 ||
          !descriptor || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
        throw std::invalid_argument(
            "Syncer 默认 CPU 黑帧需要有效的尺寸和软件像素格式");
      }
    }
  }
  if (audio &&
      (config.encoder.audio_sample_rate <= 0 ||
       !av_get_sample_fmt_name(config.encoder.audio_sample_format) ||
       !av_channel_layout_check(&config.encoder.audio_channel_layout))) {
    throw std::invalid_argument("Syncer 音频格式无效");
  }
}

void ValidateInput(const AvDecoderReadResult& input, bool video,
                   const SyncerConfig& config) {
  if (video ? config.encoder.video_pixel_format == AV_PIX_FMT_NONE
            : config.encoder.audio_sample_format == AV_SAMPLE_FMT_NONE) {
    throw std::invalid_argument("Syncer 输入轨道未启用");
  }
  if (input.status == AvDecoderReadStatus::kEof) {
    if (input.frame) throw std::invalid_argument("Syncer EOF 不应包含帧");
    return;
  }
  if (input.status != AvDecoderReadStatus::kFrame || !input.frame ||
      !input.frame->get() || !input.frame->get()->data[0] ||
      input.time_base.num <= 0 || input.time_base.den <= 0 ||
      input.frame->get()->pts == AV_NOPTS_VALUE) {
    throw std::invalid_argument("Syncer 需要有效的帧和时间戳");
  }
  const auto* frame = input.frame->get();
  const auto pts_us = av_rescale_q(frame->pts, input.time_base, kMicroseconds);
  if (pts_us == AV_NOPTS_VALUE ||
      pts_us == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("Syncer 输入时间戳超出可表示范围");
  }
  if (video) {
    if (!ValidVideoFrame(*frame) ||
        av_rescale_q(1, av_inv_q(config.encoder.fps), input.time_base) <= 0) {
      throw std::invalid_argument("Syncer 视频帧或帧周期无效");
    }
  } else if (frame->nb_samples <= 0 ||
             frame->format != config.encoder.audio_sample_format ||
             frame->sample_rate != config.encoder.audio_sample_rate ||
             av_channel_layout_compare(&frame->ch_layout,
                                       &config.encoder.audio_channel_layout) !=
                 0) {
    throw std::invalid_argument("Syncer 音频帧与配置格式不符");
  }
}

}  // namespace

class Syncer::Impl final {
 public:
  explicit Impl(SyncerConfig config) : config_(std::move(config)) {
    ValidateConfig(config_);
    if (config_.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&audio_format_->ch_layout,
                                 &config_.encoder.audio_channel_layout),
          "复制 Syncer 音频布局");
      config_.encoder.audio_channel_layout = audio_format_->ch_layout;
    }
    timing_ = std::make_unique<internal::SyncerTiming>(config_, this);
    StartLatency();
  }

  SyncerStep Step(bool video, const AvDecoderReadResult* head,
                  std::int64_t now_us, const OnOutput& on_output) {
    if (video ? config_.encoder.video_pixel_format == AV_PIX_FMT_NONE
              : config_.encoder.audio_sample_format == AV_SAMPLE_FMT_NONE) {
      throw std::invalid_argument("Syncer 输入轨道未启用");
    }
    if (now_us < 0) throw std::invalid_argument("Syncer 时间不得为负数");
    if (head) ValidateInput(*head, video, config_);
    auto& latency = video ? video_latency_ : audio_latency_;
    if (!latency.enabled())
      return timing_->Step(video, head, now_us, on_output);
    const auto started = latency.Begin();
    std::chrono::nanoseconds callback_time{};
    // Measure this component's work, excluding synchronous downstream code.
    auto step =
        timing_->Step(video, head, now_us, [&](const ffmpeg::Frame& frame) {
          if (!on_output) return true;
          const auto before = internal::StageLatency::Clock::now();
          const bool accepted = on_output(frame);
          callback_time += internal::StageLatency::Clock::now() - before;
          return accepted;
        });
    if (step.frame) {
      latency.Observe(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              internal::StageLatency::Clock::now() - started - callback_time)
              .count());
    }
    return step;
  }

  void Reset() {
    timing_ = std::make_unique<internal::SyncerTiming>(config_, this);
    StartLatency();
  }

 private:
  void StartLatency() {
    if (config_.encoder.video_pixel_format != AV_PIX_FMT_NONE)
      video_latency_.Start(PerformanceStage::kSyncerVideo, this);
    if (config_.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE)
      audio_latency_.Start(PerformanceStage::kSyncerAudio, this);
  }

  SyncerConfig config_;
  ffmpeg::Frame audio_format_;
  std::unique_ptr<internal::SyncerTiming> timing_;
  internal::StageLatency video_latency_;
  internal::StageLatency audio_latency_;
};

Syncer::Syncer(SyncerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Syncer::~Syncer() = default;
SyncerStep Syncer::Step(bool video, const AvDecoderReadResult* head,
                        std::int64_t now_us, const OnOutput& on_output) {
  return impl_->Step(video, head, now_us, on_output);
}
void Syncer::Reset() { impl_->Reset(); }

}  // namespace mw::streamer

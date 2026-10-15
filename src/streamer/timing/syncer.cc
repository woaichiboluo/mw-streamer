#include "mw/streamer/timing/syncer.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/thread.h"
#include "syncer_timing.h"

namespace mw::streamer {
namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;
constexpr AVRational kMicroseconds{1, 1000000};

bool ValidVideoFrame(const AVFrame& frame) {
  return frame.data[0] && frame.width > 0 && frame.height > 0 &&
         frame.nb_samples == 0 &&
         av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
}

void ValidateConfig(const SyncerConfig& config) {
  const bool video = config.encoder.video_pixel_format != AV_PIX_FMT_NONE;
  const bool audio = config.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE;
  if ((!video && !audio) || config.input_capacity == 0 ||
      config.negative_time_tolerance.count() < 0 ||
      config.discontinuity_threshold.count() <= 0 ||
      config.jitter_margin.count() < 0 ||
      config.jitter_margin >= config.discontinuity_threshold ||
      config.timeout.count() <= 0 || config.freeze.count() < 0 ||
      config.max_streams_diff.count() <= 0 ||
      (config.negative_time_discard &&
       (config.negative_time_discard->count() < 0 ||
        *config.negative_time_discard >= config.negative_time_tolerance))) {
    throw std::invalid_argument("Syncer 轨道、队列容量或时间阈值无效");
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
  }

  ~Impl() { Stop(); }

  void SetOnVideoFrame(OnVideoFrame callback) {
    std::lock_guard lock(mutex_);
    CheckStopped();
    on_video_frame_ = std::move(callback);
  }
  void SetOnAudioFrame(OnAudioFrame callback) {
    std::lock_guard lock(mutex_);
    CheckStopped();
    on_audio_frame_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    std::lock_guard lock(mutex_);
    CheckStopped();
    on_error_ = std::move(callback);
  }

  bool Start() {
    std::lock_guard lock(mutex_);
    if (started_) {
      MW_LOG_DEBUG("streamer", "Syncer[{}] 忽略重复启动", fmt::ptr(this));
      return false;
    }
    timing_ = std::make_unique<internal::SyncerTiming>(config_, this);
    Clear();
    error_ = {};
    epoch_ = Clock::now();
    accepting_ = true;
    started_ = true;
    if (worker_.Start([this] { Run(); })) {
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 已启动: video={} audio={} fps={}/{} "
                  "audio_rate={} audio_format={} channels={} capacity={}",
                  fmt::ptr(this),
                  config_.encoder.video_pixel_format != AV_PIX_FMT_NONE,
                  config_.encoder.audio_sample_format != AV_SAMPLE_FMT_NONE,
                  config_.encoder.fps.num, config_.encoder.fps.den,
                  config_.encoder.audio_sample_rate,
                  static_cast<int>(config_.encoder.audio_sample_format),
                  config_.encoder.audio_channel_layout.nb_channels,
                  config_.input_capacity);
      MW_LOG_DEBUG("streamer",
                   "Syncer[{}] 调度配置: late_tolerance_us={} discard_us={} "
                   "discontinuity_us={} jitter_margin_us={} timeout_us={} "
                   "freeze_us={} max_streams_diff_us={} start_ts_us={} "
                   "forward_start_shift={}",
                   fmt::ptr(this), config_.negative_time_tolerance.count(),
                   config_.negative_time_discard
                       ? config_.negative_time_discard->count()
                       : -1,
                   config_.discontinuity_threshold.count(),
                   config_.jitter_margin.count(), config_.timeout.count(),
                   config_.freeze.count(), config_.max_streams_diff.count(),
                   config_.start_ts.count(), config_.forward_start_shift);
      return true;
    }
    accepting_ = false;
    started_ = false;
    MW_LOG_ERROR("streamer", "Syncer[{}] 创建调度线程失败", fmt::ptr(this));
    return false;
  }

  bool Push(const AvDecoderReadResult& input, bool video) {
    auto& track = video ? video_ : audio_;
    std::unique_lock lock(mutex_);
    if (!accepting_) {
      MW_LOG_DEBUG("streamer",
                   "Syncer[{}] 拒绝输入: track={} generation={} 已停止或失败",
                   fmt::ptr(this), video ? "video" : "audio", input.generation);
      return false;
    }
    ValidateInput(input, video, config_);
    const bool blocked = track.inputs.size() == config_.input_capacity;
    Clock::time_point wait_started;
    if (blocked) {
      wait_started = Clock::now();
      MW_LOG_DEBUG("streamer",
                   "Syncer[{}] 队列已满，等待空间: track={} generation={} "
                   "queue={}/{}",
                   fmt::ptr(this), video ? "video" : "audio", input.generation,
                   track.inputs.size(), config_.input_capacity);
    }
    track.space_available.wait(lock, [&] {
      return !accepting_ || track.inputs.size() < config_.input_capacity;
    });
    if (blocked) {
      MW_LOG_DEBUG(
          "streamer",
          "Syncer[{}] 队列等待结束: track={} generation={} "
          "wait_us={} cancelled={} queue={}/{}",
          fmt::ptr(this), video ? "video" : "audio", input.generation,
          std::chrono::duration_cast<Microseconds>(Clock::now() - wait_started)
              .count(),
          !accepting_, track.inputs.size(), config_.input_capacity);
    }
    if (!accepting_) return false;
    if ((submitted_generation_ && input.generation < *submitted_generation_) ||
        (track.generation == input.generation && track.eof)) {
      throw std::invalid_argument("Syncer 输入代次或 EOF 顺序无效");
    }
    track.inputs.push_back(input);
    submitted_generation_ = input.generation;
    track.generation = input.generation;
    track.eof = input.status == AvDecoderReadStatus::kEof;
    MW_LOG_TRACE("streamer",
                 "Syncer[{}] 接受输入: track={} kind={} generation={} "
                 "stream={} pts={} time_base={}/{} end_pts={} samples={} "
                 "queue={}/{}",
                 fmt::ptr(this), video ? "video" : "audio",
                 track.eof ? "eof" : "frame", input.generation,
                 input.stream_index,
                 input.frame ? (*input.frame)->pts : AV_NOPTS_VALUE,
                 input.time_base.num, input.time_base.den, input.end_pts,
                 input.frame ? (*input.frame)->nb_samples : 0,
                 track.inputs.size(), config_.input_capacity);
    ++revision_;
    changed_.notify_one();
    return true;
  }

  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

  void RequestStop() noexcept {
    std::lock_guard lock(mutex_);
    if (accepting_) {
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 请求停止: video_queue={} audio_queue={}",
                  fmt::ptr(this), video_.inputs.size(), audio_.inputs.size());
    }
    accepting_ = false;
    changed_.notify_all();
    video_.space_available.notify_all();
    audio_.space_available.notify_all();
  }

  void Stop() noexcept {
    RequestStop();
    worker_.Join();
    std::lock_guard lock(mutex_);
    if (started_) {
      MW_LOG_INFO("streamer",
                  "Syncer[{}] 已停止，清空未交付输入: video={} audio={}",
                  fmt::ptr(this), video_.inputs.size(), audio_.inputs.size());
    }
    Clear();
    started_ = false;
  }

 private:
  struct Track {
    std::deque<AvDecoderReadResult> inputs;
    std::condition_variable space_available;
    std::optional<std::uint64_t> generation;
    bool eof = false;
  };

  void CheckStopped() const {
    if (started_) {
      throw std::logic_error("Syncer 回调只能在 Stop 后设置");
    }
  }

  void Clear() {
    for (auto* track : {&video_, &audio_}) {
      track->inputs.clear();
      track->generation.reset();
      track->eof = false;
    }
    submitted_generation_.reset();
    ++revision_;
  }

  std::int64_t Now() const {
    return std::chrono::duration_cast<Microseconds>(Clock::now() - epoch_)
        .count();
  }

  internal::SyncerStep Step(bool video) {
    auto& track = video ? video_ : audio_;
    auto result = timing_->Step(
        video, track.inputs.empty() ? nullptr : &track.inputs.front(), Now());
    if (result.consumed) {
      const auto& input = track.inputs.front();
      MW_LOG_TRACE("streamer",
                   "Syncer[{}] 消费输入: track={} generation={} kind={} "
                   "pts={} time_base={}/{} produced_frame={} remaining={}",
                   fmt::ptr(this), video ? "video" : "audio", input.generation,
                   input.status == AvDecoderReadStatus::kEof ? "eof" : "frame",
                   input.frame ? (*input.frame)->pts : AV_NOPTS_VALUE,
                   input.time_base.num, input.time_base.den,
                   result.frame.has_value(), track.inputs.size() - 1);
      track.inputs.pop_front();
      track.space_available.notify_one();
    }
    return result;
  }

  void Run() noexcept {
    try {
      std::unique_lock lock(mutex_);
      while (accepting_) {
        const auto observed = revision_;
        auto wake_us = std::numeric_limits<std::int64_t>::max();
        for (const bool video : {true, false}) {
          if (video
                  ? config_.encoder.video_pixel_format == AV_PIX_FMT_NONE
                  : config_.encoder.audio_sample_format == AV_SAMPLE_FMT_NONE) {
            continue;
          }
          auto result = Step(video);
          wake_us = std::min(wake_us, result.wake_us);
          if (result.frame) {
            const auto output_pts = (*result.frame)->pts;
            const auto output_time_base = (*result.frame)->time_base;
            MW_LOG_TRACE("streamer",
                         "Syncer[{}] 开始交付: track={} pts={} time_base={}/{} "
                         "duration={} samples={} now_us={} video_queue={} "
                         "audio_queue={}",
                         fmt::ptr(this), video ? "video" : "audio", output_pts,
                         output_time_base.num, output_time_base.den,
                         (*result.frame)->duration, (*result.frame)->nb_samples,
                         Now(), video_.inputs.size(), audio_.inputs.size());
            lock.unlock();
            const auto callback_started = Clock::now();
            if (video) {
              if (on_video_frame_) on_video_frame_(std::move(*result.frame));
            } else {
              if (on_audio_frame_) on_audio_frame_(std::move(*result.frame));
            }
            const auto callback_us = std::chrono::duration_cast<Microseconds>(
                                         Clock::now() - callback_started)
                                         .count();
            MW_LOG_TRACE("streamer",
                         "Syncer[{}] 交付完成: track={} pts={} time_base={}/{} "
                         "callback_us={}",
                         fmt::ptr(this), video ? "video" : "audio", output_pts,
                         output_time_base.num, output_time_base.den,
                         callback_us);
            if (callback_us >= 20000) {
              MW_LOG_DEBUG(
                  "streamer",
                  "Syncer[{}] 交付回调耗时较长: track={} callback_us={}",
                  fmt::ptr(this), video ? "video" : "audio", callback_us);
            }
            lock.lock();
            if (!accepting_) break;
          }
        }
        if (!accepting_) break;
        if (wake_us <= Now()) continue;
        const auto max_wait_us = std::chrono::duration_cast<Microseconds>(
                                     Clock::time_point::max() - epoch_)
                                     .count();
        const auto deadline = wake_us >= max_wait_us
                                  ? Clock::time_point::max()
                                  : epoch_ + Microseconds(wake_us);
        changed_.wait_until(lock, deadline, [&] {
          return !accepting_ || revision_ != observed;
        });
      }
      MW_LOG_DEBUG("streamer", "Syncer[{}] 调度线程退出", fmt::ptr(this));
    } catch (...) {
      const auto failure = std::current_exception();
      {
        std::lock_guard lock(mutex_);
        error_ = failure;
      }
      RequestStop();
      try {
        std::rethrow_exception(failure);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "Syncer[{}] 调度失败: {}", fmt::ptr(this),
                     error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "Syncer[{}] 调度失败: 未知异常",
                     fmt::ptr(this));
      }
      if (on_error_) {
        try {
          on_error_(failure);
        } catch (const std::exception& error) {
          MW_LOG_ERROR("streamer", "Syncer[{}] 错误通知异常: {}",
                       fmt::ptr(this), error.what());
        } catch (...) {
          MW_LOG_ERROR("streamer", "Syncer[{}] 错误通知发生未知异常",
                       fmt::ptr(this));
        }
      }
    }
  }

  SyncerConfig config_;
  ffmpeg::Frame audio_format_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Track video_;
  Track audio_;
  std::optional<std::uint64_t> submitted_generation_;
  std::uint64_t revision_ = 0;
  bool accepting_ = false;
  bool started_ = false;
  std::exception_ptr error_;
  Clock::time_point epoch_;
  std::unique_ptr<internal::SyncerTiming> timing_;
  OnVideoFrame on_video_frame_;
  OnAudioFrame on_audio_frame_;
  OnError on_error_;
  internal::Thread worker_;
};

Syncer::Syncer(SyncerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Syncer::~Syncer() = default;
void Syncer::SetOnVideoFrame(OnVideoFrame callback) {
  impl_->SetOnVideoFrame(std::move(callback));
}
void Syncer::SetOnAudioFrame(OnAudioFrame callback) {
  impl_->SetOnAudioFrame(std::move(callback));
}
void Syncer::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
bool Syncer::Start() { return impl_->Start(); }
bool Syncer::PushVideo(const AvDecoderReadResult& input) {
  return impl_->Push(input, true);
}
bool Syncer::PushAudio(const AvDecoderReadResult& input) {
  return impl_->Push(input, false);
}
std::exception_ptr Syncer::error() const { return impl_->error(); }
void Syncer::RequestStop() noexcept { impl_->RequestStop(); }
void Syncer::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer

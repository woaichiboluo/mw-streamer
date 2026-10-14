#include "mw/streamer/output/output.h"

#include <chrono>
#include <cstddef>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "mw/log.h"

namespace mw::streamer {

class Output::Impl final {
 public:
  explicit Impl(std::vector<std::shared_ptr<FrameSink>> frame_sinks)
      : frame_sinks_(std::move(frame_sinks)), pending_(frame_sinks_.size()) {
    for (std::size_t index = 0; index < frame_sinks_.size(); ++index) {
      if (!frame_sinks_[index])
        throw std::invalid_argument("Output frame_sink 不能为空");
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (frame_sinks_[previous] == frame_sinks_[index])
          throw std::invalid_argument("Output frame_sink 不能重复");
      }
    }
    status_.frame_sinks.resize(frame_sinks_.size());
    clocked_.resize(frame_sinks_.size());
    frame_sink_initialized_.resize(frame_sinks_.size());
  }

  bool Start(const AvEncoderConfig& format) noexcept {
    const bool has_video = format.video_pixel_format != AV_PIX_FMT_NONE;
    const bool has_audio = format.audio_sample_format != AV_SAMPLE_FMT_NONE;
    const auto* pixel_format = av_pix_fmt_desc_get(format.video_pixel_format);
    if ((!has_video && !has_audio) || format.fps.num <= 0 ||
        format.fps.den <= 0 ||
        (has_video &&
         (format.width <= 0 || format.height <= 0 || !pixel_format ||
          (pixel_format->flags & AV_PIX_FMT_FLAG_HWACCEL))) ||
        (has_audio &&
         (format.audio_sample_rate <= 0 ||
          !av_get_sample_fmt_name(format.audio_sample_format) ||
          !av_channel_layout_check(&format.audio_channel_layout)))) {
      MW_LOG_ERROR("streamer", "Output 频道格式无效");
      return false;
    }
    if (initialized_ && !SameFormat(format)) {
      MW_LOG_ERROR("streamer", "Output 不允许改变已初始化的频道格式");
      return false;
    }
    if (!initialized_) {
      if (has_audio) {
        const int result = av_channel_layout_copy(&audio_format_->ch_layout,
                                                  &format.audio_channel_layout);
        if (result < 0) {
          MW_LOG_ERROR("streamer", "复制 Output 音频布局失败: error={}",
                       result);
          return false;
        }
      }
      format_ = format;
      format_.audio_channel_layout =
          has_audio ? audio_format_->ch_layout : AVChannelLayout{};
      initialized_ = true;
    }
    bool success = true;
    for (std::size_t index = 0; index < frame_sinks_.size(); ++index) {
      if (!frame_sinks_[index]) {
        success = false;
        MW_LOG_ERROR("streamer", "Output frame_sink 已失效，无法启动: index={}",
                     index);
        continue;
      }
      if (frame_sink_initialized_[index]) continue;
      const bool initialized = frame_sinks_[index]->Initialize(format_);
      frame_sink_initialized_[index] = initialized;
      const bool clocked =
          initialized && frame_sinks_[index]->has_synchronization_clock();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        clocked_[index] = clocked;
        status_.frame_sinks[index].state =
            initialized ? FrameSinkState::kRunning : FrameSinkState::kFailed;
      }
      if (!initialized) {
        success = false;
        MW_LOG_ERROR("streamer", "Output frame_sink 初始化失败: index={}",
                     index);
      }
    }
    running_ = success;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = success ? OutputState::kRunning : OutputState::kStopped;
    }
    return success;
  }

  bool Send(const OutputFrame& frame, const std::atomic<bool>* cancelled) {
    if (!running_) throw std::logic_error("Output 尚未成功启动");
    if (!WaitPending(cancelled)) return false;
    for (std::size_t index = 0; index < frame_sinks_.size(); ++index) {
      if (Cancelled(cancelled)) return false;
      if (!frame_sinks_[index]) continue;
      try {
        auto sent = frame_sinks_[index]->Send(frame);
        if (!sent.valid())
          throw std::logic_error("FrameSink Send 返回无效 future");
        if (sent.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::deferred)
          throw std::logic_error("FrameSink Send 不允许 deferred future");
        pending_[index] = std::move(sent);
      } catch (...) {
        Fail(index, std::current_exception());
      }
    }
    return WaitPending(cancelled);
  }

  bool HasSynchronizationClock() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != OutputState::kRunning) return false;
    for (std::size_t index = 0; index < clocked_.size(); ++index) {
      if (status_.frame_sinks[index].state == FrameSinkState::kRunning &&
          clocked_[index])
        return true;
    }
    return false;
  }

  OutputStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

 private:
  static bool Cancelled(const std::atomic<bool>* cancelled) {
    return cancelled && cancelled->load();
  }

  bool SameFormat(const AvEncoderConfig& format) const {
    return av_cmp_q(format.fps, format_.fps) == 0 &&
           format.video_pixel_format == format_.video_pixel_format &&
           (format.video_pixel_format == AV_PIX_FMT_NONE ||
            (format.width == format_.width &&
             format.height == format_.height)) &&
           format.audio_sample_format == format_.audio_sample_format &&
           (format.audio_sample_format == AV_SAMPLE_FMT_NONE ||
            (format.audio_sample_rate == format_.audio_sample_rate &&
             av_channel_layout_compare(&format.audio_channel_layout,
                                       &format_.audio_channel_layout) == 0));
  }

  bool WaitPending(const std::atomic<bool>* cancelled) {
    for (std::size_t index = 0; index < pending_.size(); ++index) {
      auto& sent = pending_[index];
      if (!sent.valid()) continue;
      while (true) {
        if (Cancelled(cancelled)) return false;
        if (sent.wait_for(std::chrono::milliseconds(1)) ==
            std::future_status::ready)
          break;
      }
      try {
        if (!sent.get()) throw std::runtime_error("FrameSink 拒绝继续输出");
      } catch (...) {
        Fail(index, std::current_exception());
      }
    }
    return !Cancelled(cancelled);
  }

  void Fail(std::size_t index, std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.frame_sinks[index] = {FrameSinkState::kFailed, std::move(error)};
    }
    MW_LOG_ERROR("streamer", "Output frame_sink 失败并停止分发: index={}",
                 index);
    frame_sinks_[index].reset();
  }

  std::vector<std::shared_ptr<FrameSink>> frame_sinks_;
  // Destroy pending handles before frame sinks; frame sink destructors finish
  // work.
  std::vector<std::future<bool>> pending_;
  AvEncoderConfig format_;
  ffmpeg::Frame audio_format_;
  bool initialized_ = false;
  bool running_ = false;
  std::vector<bool> frame_sink_initialized_;
  mutable std::mutex mutex_;
  OutputStatus status_;
  std::vector<bool> clocked_;
};

Output::Output(std::vector<std::shared_ptr<FrameSink>> frame_sinks)
    : impl_(std::make_unique<Impl>(std::move(frame_sinks))) {}
Output::~Output() = default;
bool Output::Start(const AvEncoderConfig& format) noexcept {
  return impl_->Start(format);
}
bool Output::Send(const OutputFrame& frame,
                  const std::atomic<bool>* cancelled) {
  return impl_->Send(frame, cancelled);
}
bool Output::HasSynchronizationClock() const {
  return impl_->HasSynchronizationClock();
}
OutputStatus Output::status() const { return impl_->status(); }

}  // namespace mw::streamer

#include "mw/streamer/pipeline/live_pipeline.h"

#include <mutex>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace mw::streamer {
namespace {

SchedulerConfig MakeSchedulerConfig(const LivePipelineConfig& config) {
  SchedulerConfig scheduler;
  scheduler.syncer.encoder = config.encoder;
  scheduler.syncer.backup_video = config.backup_video;
  scheduler.syncer.options = config.syncer;
  scheduler.realtime = config.realtime;
  scheduler.output_samples_per_frame = config.output_samples_per_frame;
  return scheduler;
}

}  // namespace

class LivePipeline::Impl final {
 public:
  Impl(const ffmpeg::HwDeviceContext& device, LivePipelineConfig config)
      : config_(std::move(config)),
        source_(std::make_shared<AvMediaSource>(device, config_.input)),
        scheduler_(source_, MakeSchedulerConfig(config_)),
        encoder_(device),
        remuxer_(config_.remuxer) {
    auto& target = config_.encoder;
    if (target.audio_sample_format != AV_SAMPLE_FMT_NONE) {
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&audio_format_->ch_layout,
                                 &target.audio_channel_layout),
          "复制 LivePipeline 音频布局");
      target.audio_channel_layout = audio_format_->ch_layout;
    }
    source_->SetOnStateChanged([this](const AvMediaSourceStatus& status) {
      MW_LOG_DEBUG(
          "streamer", "LivePipeline 输入状态变化: instance={} state={}",
          static_cast<const void*>(this), static_cast<int>(status.state));
      if (status.state == AvMediaSourceState::kFailed) Fail(status.error);
    });
    source_->SetOnPacket([this](const ffmpeg::Packet& packet) {
      if (on_input_packet_) on_input_packet_(packet);
    });
    scheduler_.SetOnVideoFrame([this](const ffmpeg::Frame& frame) {
      return encoder_.TryPushVideo(frame);
    });
    scheduler_.SetOnAudioFrame([this](const ffmpeg::Frame& frame) {
      return encoder_.TryPushAudio(frame);
    });
    encoder_.SetWriteSignal(true, scheduler_.output_signal(true));
    encoder_.SetWriteSignal(false, scheduler_.output_signal(false));
    scheduler_.SetOnError([this](std::exception_ptr error) { Fail(error); });
    encoder_.SetOnPacket([this](const ffmpeg::Packet& packet) {
      if (on_output_packet_) on_output_packet_(packet);
      remuxer_.SubmitPacket(packet);
    });
    encoder_.SetOnError([this](std::exception_ptr error) { Fail(error); });
    remuxer_.SetOnError([this](std::exception_ptr error) { Fail(error); });
  }

  ~Impl() { Stop(); }

  void SetOnInputPacket(OnPacket callback) {
    CheckStopped();
    on_input_packet_ = std::move(callback);
  }
  void SetOnOutputPacket(OnPacket callback) {
    CheckStopped();
    on_output_packet_ = std::move(callback);
  }
  void SetOnError(OnError callback) {
    CheckStopped();
    on_error_ = std::move(callback);
  }
  AvMediaFilter& media_filter() {
    CheckStopped();
    return scheduler_.media_filter();
  }

  std::string AddPushUrl(const std::string& url) {
    return remuxer_.AddPushUrl(url);
  }
  void AddRtspPublish(const std::string& ip, std::uint16_t port, bool tls) {
    remuxer_.AddRtspPublish(ip, port, tls);
  }
  std::string AddHlsPublish(const std::string& ip, std::uint16_t port,
                            bool tls) {
    return remuxer_.AddHlsPublish(ip, port, tls);
  }
  Remuxer::OutputInfo output_info() const { return remuxer_.output_info(); }

  bool Start(std::string_view url) {
    {
      std::lock_guard lock(mutex_);
      if (started_) return false;
      started_ = true;
      state_ = LivePipelineState::kStarting;
      error_ = {};
    }
    MW_LOG_INFO("streamer", "LivePipeline 开始启动: instance={}",
                static_cast<const void*>(this));
    try {
      if (!encoder_.Start(config_.encoder)) {
        if (const auto failure = encoder_.error())
          std::rethrow_exception(failure);
        throw std::runtime_error("LivePipeline 编码器启动失败");
      }
      MW_LOG_DEBUG("streamer", "LivePipeline 编码器启动完成: instance={}",
                   static_cast<const void*>(this));
      CheckFailure();
      if (!remuxer_.Start(encoder_.streams()))
        throw std::runtime_error("LivePipeline Remuxer 启动失败");
      MW_LOG_DEBUG("streamer", "LivePipeline Remuxer 启动完成: instance={}",
                   static_cast<const void*>(this));
      CheckFailure();
      if (!source_->Start(url))
        throw std::runtime_error("LivePipeline MediaSource 启动失败");
      MW_LOG_DEBUG("streamer", "LivePipeline MediaSource 启动完成: instance={}",
                   static_cast<const void*>(this));
      CheckFailure();
      if (!scheduler_.Start()) {
        if (const auto failure = scheduler_.error())
          std::rethrow_exception(failure);
        throw std::runtime_error("LivePipeline Scheduler 启动失败");
      }
      MW_LOG_DEBUG("streamer", "LivePipeline Scheduler 启动完成: instance={}",
                   static_cast<const void*>(this));
      bool cancelled;
      {
        std::lock_guard lock(mutex_);
        if (error_) std::rethrow_exception(error_);
        cancelled = state_ == LivePipelineState::kStopping;
        if (!cancelled) state_ = LivePipelineState::kRunning;
      }
      // Component Start calls reset their own cancellation flags. Honour a
      // pipeline stop requested at any earlier point in initialization.
      if (cancelled) {
        Stop();
        return false;
      }
      MW_LOG_INFO("streamer", "LivePipeline 已启动: instance={}",
                  static_cast<const void*>(this));
      return true;
    } catch (...) {
      Fail(std::current_exception());
      Stop();
      return false;
    }
  }

  LivePipelineState state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }
  AvMediaSourceStatus input_status() const { return source_->status(); }
  std::vector<Remuxer::PushStatus> remuxer_status() const {
    return remuxer_.push_statuses();
  }
  LivePipelineStatus status() const {
    LivePipelineStatus result;
    {
      std::lock_guard lock(mutex_);
      result.state = state_;
      result.error = error_;
    }
    // push_statuses synchronizes with the output poller, whose error callback
    // may need mutex_. Never hold it while querying component snapshots.
    result.input = input_status();
    result.remuxer = remuxer_status();
    return result;
  }
  std::exception_ptr error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

  void RequestStop() noexcept {
    bool requested = false;
    {
      std::lock_guard lock(mutex_);
      requested = started_ && state_ != LivePipelineState::kStopping &&
                  state_ != LivePipelineState::kFailed;
      if (started_ && state_ != LivePipelineState::kFailed)
        state_ = LivePipelineState::kStopping;
    }
    if (requested)
      MW_LOG_DEBUG("streamer", "LivePipeline 请求停止: instance={}",
                   static_cast<const void*>(this));
    scheduler_.RequestStop();
    source_->RequestStop();
  }
  void Stop() noexcept {
    bool was_started;
    {
      std::lock_guard lock(mutex_);
      was_started = started_;
    }
    RequestStop();
    if (was_started)
      MW_LOG_DEBUG("streamer", "LivePipeline 等待调度与输入停止: instance={}",
                   static_cast<const void*>(this));
    scheduler_.Stop();
    source_->Stop();
    // Keep Remuxer accepting until all delayed encoder packets are delivered.
    if (was_started)
      MW_LOG_DEBUG("streamer", "LivePipeline 排空编码器与输出: instance={}",
                   static_cast<const void*>(this));
    encoder_.Stop();
    remuxer_.Stop();
    {
      std::lock_guard lock(mutex_);
      started_ = false;
      state_ =
          error_ ? LivePipelineState::kFailed : LivePipelineState::kStopped;
    }
    if (was_started)
      MW_LOG_INFO("streamer", "LivePipeline 已停止: instance={} failed={}",
                  static_cast<const void*>(this), static_cast<bool>(error()));
  }

 private:
  void CheckStopped() const {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("配置 LivePipeline 前需要 Stop");
  }
  void CheckFailure() const {
    if (const auto failure = error()) std::rethrow_exception(failure);
  }
  void Fail(std::exception_ptr error) {
    {
      std::lock_guard lock(mutex_);
      if (error_) return;
      error_ = error;
      state_ = LivePipelineState::kFailed;
    }
    scheduler_.RequestStop();
    source_->RequestStop();
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& failure) {
      MW_LOG_ERROR("streamer",
                   "LivePipeline 不可恢复错误: instance={} error={}",
                   static_cast<const void*>(this), failure.what());
    } catch (...) {
      MW_LOG_ERROR("streamer",
                   "LivePipeline 不可恢复错误: instance={} 未知异常",
                   static_cast<const void*>(this));
    }
    if (on_error_) {
      try {
        on_error_(error);
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer",
                     "LivePipeline OnError 回调失败: instance={} error={}",
                     static_cast<const void*>(this), failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer",
                     "LivePipeline OnError 回调失败: instance={} 未知异常",
                     static_cast<const void*>(this));
      }
    }
  }

  LivePipelineConfig config_;
  ffmpeg::Frame audio_format_;
  std::shared_ptr<AvMediaSource> source_;
  Scheduler scheduler_;
  AvEncoder encoder_;
  Remuxer remuxer_;
  mutable std::mutex mutex_;
  LivePipelineState state_ = LivePipelineState::kStopped;
  bool started_ = false;
  std::exception_ptr error_;
  OnPacket on_input_packet_;
  OnPacket on_output_packet_;
  OnError on_error_;
};

LivePipeline::LivePipeline(LivePipelineConfig config)
    : LivePipeline(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCpu),
                   std::move(config)) {}
LivePipeline::LivePipeline(const ffmpeg::HwDeviceContext& device,
                           LivePipelineConfig config)
    : impl_(std::make_unique<Impl>(device, std::move(config))) {}
LivePipeline::~LivePipeline() = default;
void LivePipeline::SetOnInputPacket(OnPacket callback) {
  impl_->SetOnInputPacket(std::move(callback));
}
void LivePipeline::SetOnOutputPacket(OnPacket callback) {
  impl_->SetOnOutputPacket(std::move(callback));
}
void LivePipeline::SetOnError(OnError callback) {
  impl_->SetOnError(std::move(callback));
}
AvMediaFilter& LivePipeline::media_filter() { return impl_->media_filter(); }
std::string LivePipeline::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void LivePipeline::AddRtspPublish(const std::string& ip, std::uint16_t port,
                                  bool tls) {
  impl_->AddRtspPublish(ip, port, tls);
}
std::string LivePipeline::AddHlsPublish(const std::string& ip,
                                        std::uint16_t port, bool tls) {
  return impl_->AddHlsPublish(ip, port, tls);
}
Remuxer::OutputInfo LivePipeline::output_info() const {
  return impl_->output_info();
}
bool LivePipeline::Start(std::string_view url) { return impl_->Start(url); }
LivePipelineState LivePipeline::state() const { return impl_->state(); }
AvMediaSourceStatus LivePipeline::input_status() const {
  return impl_->input_status();
}
std::vector<Remuxer::PushStatus> LivePipeline::remuxer_status() const {
  return impl_->remuxer_status();
}
LivePipelineStatus LivePipeline::status() const { return impl_->status(); }
std::exception_ptr LivePipeline::error() const { return impl_->error(); }
void LivePipeline::RequestStop() noexcept { impl_->RequestStop(); }
void LivePipeline::Stop() noexcept { impl_->Stop(); }

}  // namespace mw::streamer

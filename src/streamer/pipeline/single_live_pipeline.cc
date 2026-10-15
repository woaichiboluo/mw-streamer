#include "mw/streamer/pipeline/single_live_pipeline.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/output/encode_push_sink.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace mw::streamer {
namespace {

template <typename Callback, typename... Args>
void Notify(const char* name, const Callback& callback,
            Args&&... args) noexcept {
  if (!callback) return;
  try {
    callback(std::forward<Args>(args)...);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "SingleLivePipeline {} 回调失败: {}", name,
                 error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "SingleLivePipeline {} 回调发生未知异常", name);
  }
}

}  // namespace

class SingleLivePipeline::Impl final {
 public:
  Impl(const ffmpeg::HwDeviceContext& device, SingleLivePipelineConfig config)
      : device_(device.Ref()), config_(std::move(config)) {
    ffmpeg::FfmpegException::throwIfError(
        av_channel_layout_copy(&layout_->ch_layout,
                               &config_.output.audio_channel_layout),
        "复制 SingleLivePipeline 音频布局");
    config_.output.audio_channel_layout = layout_->ch_layout;
  }

  ~Impl() { Stop(); }

  template <typename Callback>
  void Configure(Callback& slot, Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_)
      throw std::logic_error("配置 SingleLivePipeline 回调前需要 Stop");
    slot = std::move(callback);
  }

  bool Start(std::string_view url) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_) {
        MW_LOG_ERROR("streamer", "SingleLivePipeline 启动前需要 Stop");
        return false;
      }
      active_ = true;
      stop_requested_ = ended_notified_ = false;
      state_ = SingleLivePipelineState::kStopped;
      input_status_ = {};
      sink_status_ = {};
      error_ = {};
    }
    try {
      if (url.empty() || url.find('\0') != std::string_view::npos ||
          url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
        throw std::invalid_argument("SingleLivePipeline 输入地址无效");

      auto sink = EnsureSink();
      AvMediaInputConfig input_config;
      input_config.proxy = config_.input;
      input_config.video_decoder = config_.video_decoder;
      input_config.audio_decoder = config_.audio_decoder;
      input_config.output = config_.output;
      auto input = std::make_shared<AvMediaInput>(device_, input_config);
      input->SetOnStateChanged(
          [this](const AvMediaInputStatus& status) { InputChanged(status); });
      auto scheduler =
          std::make_unique<Scheduler>(SchedulerConfig{config_.output});
      auto output = std::make_shared<Output>(
          std::vector<std::shared_ptr<FrameSink>>{sink});
      scheduler->SetOutput(output);
      if (on_video_) {
        scheduler->SetOnVideo(
            [this](const Scheduler::Inputs& inputs, const SchedulerTick&) {
              const auto& frame = inputs.front().frame.video;
              return frame && frame->get()->data[0] ? on_video_(*frame)
                                                    : ffmpeg::Frame{};
            });
      }
      if (on_audio_) {
        scheduler->SetOnAudio(
            [this](const Scheduler::Inputs& inputs, const SchedulerTick&) {
              const auto& frame = inputs.front().frame.audio;
              return frame && frame->get()->data[0] ? on_audio_(*frame)
                                                    : ffmpeg::Frame{};
            });
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        input_ = std::move(input);
        scheduler_ = std::move(scheduler);
        output_ = std::move(output);
      }

      if (on_initialize_) on_initialize_();
      business_initialized_ = true;
      if (Cancelled())
        throw std::runtime_error("SingleLivePipeline 启动被取消");
      if (!output_->Start(config_.output))
        throw std::runtime_error("SingleLivePipeline 输出初始化失败");
      if (Cancelled())
        throw std::runtime_error("SingleLivePipeline 启动被取消");
      if (!input_->Start(url))
        throw std::runtime_error("SingleLivePipeline 输入启动失败");
      if (!Cancelled() && !scheduler_->Start({input_}))
        throw std::runtime_error("SingleLivePipeline 调度器启动失败");
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stop_requested_) state_ = SingleLivePipelineState::kRunning;
      }
      // Component Start resets its cancellation flag. Reapply a request that
      // raced with startup, without overwriting an asynchronously failed state.
      if (Cancelled()) RequestStop();
      return true;
    } catch (...) {
      const auto error = std::current_exception();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_) error_ = error;
        state_ = SingleLivePipelineState::kFailed;
      }
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& failure) {
        MW_LOG_ERROR("streamer", "SingleLivePipeline 启动失败: {}",
                     failure.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "SingleLivePipeline 启动发生未知异常");
      }
      Stop();
      return false;
    }
  }

  void RequestStop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
    if (scheduler_) scheduler_->RequestStop();
    if (input_) input_->RequestStop();
  }

  void Stop() noexcept {
    RequestStop();
    std::unique_ptr<Scheduler> scheduler;
    std::shared_ptr<AvMediaInput> input;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler = std::move(scheduler_);
      input = std::move(input_);
    }
    if (scheduler) scheduler->Stop();
    scheduler.reset();
    if (input) input->Stop();
    input.reset();

    std::shared_ptr<Output> output;
    std::shared_ptr<EncodePushSink> sink;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (output_ && sink_status_.state != FrameSinkState::kFailed)
        sink_status_ = output_->status().frame_sinks.front();
      output = std::move(output_);
      sink = std::move(sink_);
      // Queries already running may use the Sink, but new queries now see no
      // Sink. Wait before destruction so business Stop cannot precede drain.
      queries_finished_.wait(lock, [this] { return push_queries_ == 0; });
    }
    if (sink) {
      auto info = sink->output_info();
      std::lock_guard<std::mutex> lock(mutex_);
      output_info_ = std::move(info);
    }
    output.reset();
    sink.reset();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != SingleLivePipelineState::kFailed)
        state_ = SingleLivePipelineState::kStopped;
      if (sink_status_.state != FrameSinkState::kFailed) sink_status_ = {};
    }
    if (business_initialized_) {
      business_initialized_ = false;
      Notify("OnStop", on_stop_);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
    }
  }

  SingleLivePipelineState state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  AvMediaInputStatus input_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return input_status_;
  }

  FrameSinkStatus sink_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (output_ && sink_status_.state != FrameSinkState::kFailed)
      return output_->status().frame_sinks.front();
    return sink_status_;
  }

  std::exception_ptr error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  Remuxer::OutputInfo output_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_info_;
  }

  std::vector<Remuxer::PushStatus> push_statuses() const {
    EncodePushSink* sink;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!sink_) return {};
      sink = sink_.get();
      ++push_queries_;
    }
    // Never hold the pipeline mutex while waiting for the output poller: an
    // error callback on that poller may need to publish pipeline failure.
    try {
      auto result = sink->push_statuses();
      FinishQuery();
      return result;
    } catch (...) {
      FinishQuery();
      throw;
    }
  }

  std::string AddPushUrl(const std::string& url) {
    return AddTarget({TargetKind::kPush, url, {}, {}, 0, false});
  }

  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls) {
    AddTarget({TargetKind::kRtsp, app, stream, ip, port, tls});
  }

  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls) {
    return AddTarget({TargetKind::kHls, app, stream, ip, port, tls});
  }

  OnInitialize on_initialize_;
  OnStop on_stop_;
  OnEnded on_ended_;
  OnError on_error_;
  OnVideo on_video_;
  OnAudio on_audio_;

 private:
  enum class TargetKind { kPush, kRtsp, kHls };
  struct Target {
    TargetKind kind;
    std::string url_or_app;
    std::string stream;
    std::string ip;
    std::uint16_t port;
    bool tls;
  };

  static std::string ApplyTarget(EncodePushSink& sink, const Target& target) {
    switch (target.kind) {
      case TargetKind::kPush:
        return sink.AddPushUrl(target.url_or_app);
      case TargetKind::kRtsp:
        sink.AddRtspPublish(target.url_or_app, target.stream, target.ip,
                            target.port, target.tls);
        return {};
      case TargetKind::kHls:
        return sink.AddHlsPublish(target.url_or_app, target.stream, target.ip,
                                  target.port, target.tls);
    }
    return {};
  }

  std::shared_ptr<EncodePushSink> EnsureSink() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sink_) return sink_;
    }
    auto sink = std::make_shared<EncodePushSink>(device_);
    sink->SetOnError([this](std::exception_ptr error) { Fail(error, true); });
    for (const auto& target : targets_) ApplyTarget(*sink, target);
    auto info = sink->output_info();
    std::lock_guard<std::mutex> lock(mutex_);
    output_info_ = std::move(info);
    sink_ = sink;
    return sink;
  }

  std::string AddTarget(Target target) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_ && stop_requested_)
        throw std::logic_error("SingleLivePipeline 停止后才能添加输出");
    }
    auto sink = EnsureSink();
    auto targets = targets_;
    const bool duplicate =
        target.kind == TargetKind::kPush &&
        std::any_of(targets.begin(), targets.end(),
                    [&](const Target& existing) {
                      return existing.kind == TargetKind::kPush &&
                             existing.url_or_app == target.url_or_app;
                    });
    if (!duplicate) targets.push_back(target);
    const auto result = ApplyTarget(*sink, target);
    targets_.swap(targets);
    auto info = sink->output_info();
    std::lock_guard<std::mutex> lock(mutex_);
    output_info_ = std::move(info);
    return result;
  }

  bool Cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
  }

  void FinishQuery() const {
    std::lock_guard<std::mutex> lock(mutex_);
    --push_queries_;
    queries_finished_.notify_all();
  }

  void InputChanged(const AvMediaInputStatus& status) {
    bool ended = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      input_status_ = status;
      if (status.state == AvMediaInputState::kEnded && !stop_requested_ &&
          !ended_notified_) {
        ended_notified_ = ended = true;
      }
    }
    if (status.state == AvMediaInputState::kFailed) Fail(status.error, false);
    if (ended) Notify("OnEnded", on_ended_);
  }

  void Fail(std::exception_ptr error, bool sink_failed) {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sink_failed && sink_status_.state != FrameSinkState::kFailed)
        sink_status_ = {FrameSinkState::kFailed, error};
      if (!error_) {
        error_ = error;
        state_ = SingleLivePipelineState::kFailed;
        notify = true;
      }
    }
    RequestStop();
    if (notify) Notify("OnError", on_error_, error);
  }

  ffmpeg::HwDeviceContext device_;
  SingleLivePipelineConfig config_;
  ffmpeg::Frame layout_;
  std::vector<Target> targets_;
  std::shared_ptr<AvMediaInput> input_;
  std::unique_ptr<Scheduler> scheduler_;
  std::shared_ptr<Output> output_;
  std::shared_ptr<EncodePushSink> sink_;
  bool business_initialized_ = false;
  mutable std::mutex mutex_;
  mutable std::condition_variable queries_finished_;
  mutable std::size_t push_queries_ = 0;
  bool active_ = false;
  bool stop_requested_ = false;
  bool ended_notified_ = false;
  SingleLivePipelineState state_ = SingleLivePipelineState::kStopped;
  AvMediaInputStatus input_status_;
  FrameSinkStatus sink_status_;
  Remuxer::OutputInfo output_info_;
  std::exception_ptr error_;
};

SingleLivePipeline::SingleLivePipeline(const ffmpeg::HwDeviceContext& device,
                                       SingleLivePipelineConfig config)
    : impl_(std::make_unique<Impl>(device, std::move(config))) {}
SingleLivePipeline::~SingleLivePipeline() = default;
void SingleLivePipeline::SetOnInitialize(OnInitialize callback) {
  impl_->Configure(impl_->on_initialize_, std::move(callback));
}
void SingleLivePipeline::SetOnStop(OnStop callback) {
  impl_->Configure(impl_->on_stop_, std::move(callback));
}
void SingleLivePipeline::SetOnEnded(OnEnded callback) {
  impl_->Configure(impl_->on_ended_, std::move(callback));
}
void SingleLivePipeline::SetOnError(OnError callback) {
  impl_->Configure(impl_->on_error_, std::move(callback));
}
void SingleLivePipeline::SetOnVideo(OnVideo callback) {
  impl_->Configure(impl_->on_video_, std::move(callback));
}
void SingleLivePipeline::SetOnAudio(OnAudio callback) {
  impl_->Configure(impl_->on_audio_, std::move(callback));
}
bool SingleLivePipeline::Start(std::string_view url) noexcept {
  return impl_->Start(url);
}
void SingleLivePipeline::RequestStop() noexcept { impl_->RequestStop(); }
void SingleLivePipeline::Stop() noexcept { impl_->Stop(); }
SingleLivePipelineState SingleLivePipeline::state() const {
  return impl_->state();
}
AvMediaInputStatus SingleLivePipeline::input_status() const {
  return impl_->input_status();
}
FrameSinkStatus SingleLivePipeline::sink_status() const {
  return impl_->sink_status();
}
std::exception_ptr SingleLivePipeline::error() const { return impl_->error(); }
std::string SingleLivePipeline::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void SingleLivePipeline::AddRtspPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port) {
  AddRtspPublish(app, stream, ip, port, false);
}
void SingleLivePipeline::AddRtspPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port, bool tls) {
  impl_->AddRtspPublish(app, stream, ip, port, tls);
}
std::string SingleLivePipeline::AddHlsPublish(const std::string& app,
                                              const std::string& stream,
                                              const std::string& ip,
                                              std::uint16_t port) {
  return AddHlsPublish(app, stream, ip, port, false);
}
std::string SingleLivePipeline::AddHlsPublish(const std::string& app,
                                              const std::string& stream,
                                              const std::string& ip,
                                              std::uint16_t port, bool tls) {
  return impl_->AddHlsPublish(app, stream, ip, port, tls);
}
Remuxer::OutputInfo SingleLivePipeline::output_info() const {
  return impl_->output_info();
}
std::vector<Remuxer::PushStatus> SingleLivePipeline::push_statuses() const {
  return impl_->push_statuses();
}

}  // namespace mw::streamer

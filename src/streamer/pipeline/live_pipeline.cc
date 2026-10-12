#include "mw/streamer/pipeline/live_pipeline.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/remuxer/async_remuxer.h"

namespace mw::streamer {
namespace {

bool Terminal(PipelineState state) {
  return state == PipelineState::kStopped || state == PipelineState::kEnded ||
         state == PipelineState::kFailed;
}

FfmpegInputConfig InputConfig(const LivePipelineConfig& config) {
  if (config.input.mode != InputMode::kLive) {
    throw std::invalid_argument("LivePipeline 需要 kLive 输入模式");
  }
  const auto output = config.scheduler.video_frame_rate;
  const auto encoded = config.encoder.fps;
  if (output.num <= 0 || output.den <= 0 || encoded.num <= 0 ||
      encoded.den <= 0 ||
      static_cast<std::int64_t>(output.num) * encoded.den !=
          static_cast<std::int64_t>(encoded.num) * output.den) {
    throw std::invalid_argument("LivePipeline 调度与编码帧率需要有效且一致");
  }
  return config.input;
}

}  // namespace

class LivePipeline::Impl final {
 public:
  Impl(const ffmpeg::HwDeviceContext& device, LivePipelineConfig config)
      : config_(std::move(config)),
        video_device_(device),
        input_(video_device_, InputConfig(config_)),
        scheduler_(config_.scheduler) {
    input_.SetOnReady([this](const auto& streams) { Prepare(streams); });
    input_.SetOnDecodedReady(
        [this](const auto& source) { QueueSource(source); });
    input_.SetOnStateChanged([this](auto state, int code, auto message) {
      InputChanged(state, code, message);
    });
    scheduler_.SetOnFrame(
        [this](const MediaFrame& frame) noexcept { Process(frame); });
    scheduler_.SetOnReady(
        [this](const auto& streams) { PrepareEncoding(streams); });
    scheduler_.SetOnEnded([this]() noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_ended_ = true;
      changed_.notify_all();
    });
    scheduler_.SetOnError([this](int code, auto message) noexcept {
      Fail("scheduler", code, message);
    });
    encoder_.SetOnReady(
        [this](const auto& streams) noexcept { PrepareOutput(streams); });
    encoder_.SetOnPacket(
        [this](const auto& packet) noexcept { SubmitEncoded(packet); });
    encoder_.SetOnEnded([this]() noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      encoder_ended_ = true;
      changed_.notify_all();
    });
    encoder_.SetOnError([this](int code, auto message) noexcept {
      Fail("encoder", code, message);
    });
    remuxer_.SetOnError([this](auto target, int code, auto message) {
      if (target.empty()) {
        Fail("remuxer", code, message);
      } else {
        MW_LOG_ERROR("streamer",
                     "LivePipeline 输出错误: target={}, code={}, reason={}",
                     target, code, message);
      }
    });
  }

  ~Impl() { Stop(); }

  void SetOnSelectStreams(OnSelectStreams callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    if (!callback) {
      input_.SetOnSelectStreams({});
      return;
    }
    input_.SetOnSelectStreams(
        [this, callback = std::move(callback)](const auto& streams) {
          try {
            return callback(streams);
          } catch (...) {
            Exception("input", std::current_exception());
            throw;
          }
        });
  }

  void SetOnReady(OnReady callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    processor_.SetOnReady(std::move(callback));
  }
  void SetOnVideo(OnVideo callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    processor_.SetOnVideo(std::move(callback));
  }
  void SetOnAudio(OnAudio callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    processor_.SetOnAudio(std::move(callback));
  }
  void SetOnEnded(OnEnded callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    processor_.SetOnEnded(std::move(callback));
  }
  void SetOnStop(OnStop callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    processor_.SetOnStop(std::move(callback));
  }
  void SetOnStateChanged(OnStateChanged callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    on_state_changed_ = std::move(callback);
  }

  std::string AddPushUrl(const std::string& url) {
    std::lock_guard<std::mutex> control(control_);
    std::lock_guard<std::mutex> outputs(outputs_);
    RequireAdding();
    auto result = remuxer_.AddPushUrl(url);
    UpdateOutputs();
    return result;
  }
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls) {
    std::lock_guard<std::mutex> control(control_);
    std::lock_guard<std::mutex> outputs(outputs_);
    RequireAdding();
    remuxer_.AddRtspPublish(app, stream, ip, port, tls);
    UpdateOutputs();
  }
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls) {
    std::lock_guard<std::mutex> control(control_);
    std::lock_guard<std::mutex> outputs(outputs_);
    RequireAdding();
    auto result = remuxer_.AddHlsPublish(app, stream, ip, port, tls);
    UpdateOutputs();
    return result;
  }

  void Start(std::string_view url) {
    if (url.empty() ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos)
      throw std::invalid_argument("LivePipeline 输入地址不能为空");
    {
      std::lock_guard<std::mutex> control(control_);
      const auto previous = state();
      if (previous != PipelineState::kIdle && !Terminal(previous))
        throw std::logic_error("LivePipeline 已经启动");
      if (worker_.joinable()) worker_.join();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.state = previous;
        ready_info_ = {};
        ready_ = configured_ = finish_requested_ = false;
        scheduler_started_ = false;
        input_available_ = false;
        scheduler_ended_ = encoder_ended_ = encoder_started_ = false;
        finish_state_ = PipelineState::kStopped;
        startup_error_ = nullptr;
      }
      ChangeState(PipelineState::kStarting);
      try {
        worker_ = std::thread([this] { Run(); });
      } catch (...) {
        Exception("pipeline", std::current_exception());
        StopOutput();
        ChangeState(PipelineState::kFailed);
        throw;
      }
      try {
        input_.Start(url);
      } catch (...) {
        Exception("input", std::current_exception());
      }
    }
    std::exception_ptr error;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return ready_ || Terminal(status_.state); });
      if (ready_ || (configured_ && status_.state == PipelineState::kEnded))
        return;
      error = startup_error_;
    }
    Stop();
    if (error) std::rethrow_exception(error);
    throw ffmpeg::FfmpegException(AVERROR_EXIT, "LivePipeline 启动被停止");
  }

  void Stop() {
    std::lock_guard<std::mutex> control(control_);
    if (!worker_.joinable()) {
      StopOutput();
      if (state() == PipelineState::kIdle) ChangeState(PipelineState::kStopped);
      return;
    }
    Finish(PipelineState::kStopped);
    worker_.join();
    StopOutput();
  }
  PipelineState state() const noexcept { return state_.load(); }
  PipelineStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }
  LivePipelineReadyInfo ready_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_info_;
  }

 private:
  void RequireConfigurable() {
    if (worker_.joinable())
      throw std::logic_error("配置 LivePipeline 回调前需要 Stop");
  }
  void RequireAdding() const {
    if (!Terminal(state()) && Finishing())
      throw std::logic_error("LivePipeline 停止过程中不能添加输出");
  }
  void UpdateOutputs() {
    auto info = remuxer_.output_info();
    std::lock_guard<std::mutex> lock(mutex_);
    ready_info_.output_urls = std::move(info.output_urls);
    ready_info_.rtsp_url = std::move(info.rtsp_url);
    ready_info_.hls_url = std::move(info.hls_url);
  }
  void StopOutput() {
    std::lock_guard<std::mutex> outputs(outputs_);
    remuxer_.Stop();
  }
  bool Finishing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_requested_;
  }
  bool Failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_requested_ && finish_state_ == PipelineState::kFailed;
  }
  void ChangeState(PipelineState next, bool input_event = false) {
    std::lock_guard<std::mutex> notification(notifications_);
    PipelineStatus snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if ((input_event &&
           (finish_requested_ ||
            (next == PipelineState::kRunning && !input_available_))) ||
          status_.state == next)
        return;
      status_.state = next;
      state_.store(next);
      snapshot = status_;
    }
    if (on_state_changed_) {
      try {
        on_state_changed_(snapshot);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "LivePipeline 状态回调异常: {}", error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "LivePipeline 状态回调发生未知异常");
      }
    }
    changed_.notify_all();
  }
  void Finish(PipelineState result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finish_requested_ || Terminal(status_.state)) return;
    finish_requested_ = true;
    finish_state_ = result;
    changed_.notify_all();
  }
  void Exception(std::string_view source, std::exception_ptr error) noexcept {
    try {
      std::rethrow_exception(error);
    } catch (const ffmpeg::FfmpegException& failure) {
      Fail(source, failure.error_code(), failure.what(), error);
    } catch (const std::exception& failure) {
      Fail(source, AVERROR_EXTERNAL, failure.what(), error);
    } catch (...) {
      Fail(source, AVERROR_UNKNOWN, "管线发生未知异常", error);
    }
  }
  void Fail(std::string_view source, int code, std::string_view message,
            std::exception_ptr error = {}) {
    MW_LOG_ERROR("streamer",
                 "LivePipeline 会话失败: source={}, code={}, reason={}", source,
                 code, message);
    std::lock_guard<std::mutex> lock(mutex_);
    if (Terminal(status_.state)) return;
    if (status_.error_source.empty()) {
      status_.error_source = source;
      status_.error_code = code;
      status_.error_message = message;
      if (!ready_ && !startup_error_)
        startup_error_ = error ? std::move(error)
                               : std::make_exception_ptr(
                                     ffmpeg::FfmpegException(code, message));
    }
    finish_requested_ = true;
    finish_state_ = PipelineState::kFailed;
    changed_.notify_all();
  }

  void Prepare(const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
    try {
      if (Finishing()) return;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_info_.streams = streams;
      }
    } catch (...) {
      Exception("input", std::current_exception());
    }
  }
  void PrepareEncoding(const std::vector<ffmpeg::StreamInfo>& streams) {
    std::string_view stage = "processor";
    try {
      if (Finishing()) return;
      if (!processor_.Start(streams, video_device_))
        throw std::runtime_error("Processor 启动被 OnReady 拒绝");
      if (Finishing()) return;
      stage = "encoder";
      encoder_.Start(config_.encoder, streams, video_device_);
      encoder_started_ = true;
      if (Finishing()) return;
      std::lock_guard<std::mutex> lock(mutex_);
      ready_info_.scheduled_streams = streams;
      configured_ = true;
    } catch (...) {
      Exception(stage, std::current_exception());
    }
  }
  void PrepareOutput(const std::vector<ffmpeg::StreamInfo>& streams) noexcept {
    try {
      std::lock_guard<std::mutex> outputs(outputs_);
      if (Finishing()) return;
      remuxer_.Start(streams);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_info_.encoded_streams = streams;
      }
      UpdateOutputs();
    } catch (...) {
      Exception("remuxer", std::current_exception());
    }
  }
  void QueueSource(const DecodedInputSource& source) {
    if (Finishing()) return;
    bool started;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      started = scheduler_started_;
    }
    const bool accepted =
        started ? scheduler_.QueueSource(source) : scheduler_.Start(source);
    if (!accepted) {
      Fail("scheduler", AVERROR_EXTERNAL, "Scheduler 拒绝输入源");
      return;
    }
    bool ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_started_ = true;
      input_available_ = true;
      ready = ready_;
    }
    if (ready && state() == PipelineState::kReconnecting)
      ChangeState(PipelineState::kRunning, true);
  }
  void Process(const MediaFrame& frame) noexcept {
    if (Failed()) return;
    try {
      MediaFrame processed;
      if (frame.video) processed.video = processor_.ProcessVideo(*frame.video);
      if (frame.audio) processed.audio = processor_.ProcessAudio(*frame.audio);
      const bool accepted = encoder_.SubmitFrame(processed);
      if (!accepted && !Failed())
        throw std::runtime_error("Encoder 拒绝处理后的帧");
    } catch (...) {
      Exception("encoder", std::current_exception());
    }
  }
  void SubmitEncoded(const ffmpeg::Packet& packet) noexcept {
    if (Failed()) return;
    try {
      if (!remuxer_.SubmitPacket(packet))
        throw std::runtime_error("AsyncRemuxer 拒绝编码包");
      // Repeated frames/silence during reconnection must not clear that state.
      // Only this session's first packet completes synchronous startup.
      bool first;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        first = !ready_ && !finish_requested_;
      }
      if (first) {
        ChangeState(PipelineState::kRunning, true);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!finish_requested_) ready_ = true;
        changed_.notify_all();
      }
    } catch (...) {
      Exception("remuxer", std::current_exception());
    }
  }
  void InputChanged(InputState state, int code, std::string_view message) {
    switch (state) {
      case InputState::kWaitingRetry: {
        std::lock_guard<std::mutex> lock(mutex_);
        input_available_ = false;
      }
        ChangeState(PipelineState::kReconnecting, true);
        break;
      case InputState::kEnded:
        Finish(PipelineState::kEnded);
        break;
      case InputState::kFailed:
        Fail("input", code, message);
        break;
      default:
        break;
    }
  }
  void Run() noexcept {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return finish_requested_; });
    }
    try {
      ChangeState(PipelineState::kStopping);
      bool ending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ending = finish_state_ == PipelineState::kEnded;
      }
      if (!ending) scheduler_.RequestStop();
      input_.Stop();  // Also joins initial preparation.
      bool eof;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        eof = finish_state_ == PipelineState::kEnded;
      }
      if (eof) {
        scheduler_.Drain();
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] {
          return scheduler_ended_ || finish_state_ == PipelineState::kFailed;
        });
      }
      scheduler_.Stop();
      if (eof && !Failed()) processor_.End();
      if (!eof) processor_.Stop();
      if (encoder_started_ && !Failed()) {
        encoder_.Drain();
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [this] {
          return encoder_ended_ || finish_state_ == PipelineState::kFailed;
        });
      }
      if (Failed()) encoder_.RequestCancel();
      encoder_.Stop();
      StopOutput();
      processor_.Stop();
    } catch (...) {
      Exception("pipeline", std::current_exception());
      scheduler_.Stop();
      encoder_.RequestCancel();
      encoder_.Stop();
      StopOutput();
      processor_.Stop();
    }
    PipelineState result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result = finish_state_;
    }
    ChangeState(result);
  }

  const LivePipelineConfig config_;
  ffmpeg::HwDeviceContext video_device_;
  FfmpegInput input_;
  Scheduler scheduler_;
  Processor processor_;
  Encoder encoder_;
  AsyncRemuxer remuxer_;
  std::mutex control_;
  std::mutex outputs_;
  std::mutex notifications_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::atomic<PipelineState> state_{PipelineState::kIdle};
  PipelineStatus status_;
  LivePipelineReadyInfo ready_info_;
  bool configured_ = false;
  bool scheduler_started_ = false;
  bool ready_ = false;
  bool input_available_ = false;
  bool finish_requested_ = false;
  bool scheduler_ended_ = false;
  bool encoder_ended_ = false;
  bool encoder_started_ = false;
  PipelineState finish_state_ = PipelineState::kStopped;
  std::exception_ptr startup_error_;
  OnStateChanged on_state_changed_;
};

LivePipeline::LivePipeline(LivePipelineConfig config)
    : LivePipeline(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCpu),
                   std::move(config)) {}
LivePipeline::LivePipeline(const ffmpeg::HwDeviceContext& video_device,
                           LivePipelineConfig config)
    : impl_(std::make_unique<Impl>(video_device, std::move(config))) {}
LivePipeline::~LivePipeline() = default;
void LivePipeline::SetOnSelectStreams(OnSelectStreams callback) {
  impl_->SetOnSelectStreams(std::move(callback));
}
void LivePipeline::SetOnReady(OnReady callback) {
  impl_->SetOnReady(std::move(callback));
}
void LivePipeline::SetOnVideo(OnVideo callback) {
  impl_->SetOnVideo(std::move(callback));
}
void LivePipeline::SetOnAudio(OnAudio callback) {
  impl_->SetOnAudio(std::move(callback));
}
void LivePipeline::SetOnEnded(OnEnded callback) {
  impl_->SetOnEnded(std::move(callback));
}
void LivePipeline::SetOnStop(OnStop callback) {
  impl_->SetOnStop(std::move(callback));
}
void LivePipeline::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
std::string LivePipeline::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void LivePipeline::AddRtspPublish(const std::string& app,
                                  const std::string& stream,
                                  const std::string& ip, std::uint16_t port,
                                  bool tls) {
  impl_->AddRtspPublish(app, stream, ip, port, tls);
}
std::string LivePipeline::AddHlsPublish(const std::string& app,
                                        const std::string& stream,
                                        const std::string& ip,
                                        std::uint16_t port, bool tls) {
  return impl_->AddHlsPublish(app, stream, ip, port, tls);
}
void LivePipeline::Start(std::string_view url) { impl_->Start(url); }
void LivePipeline::Stop() { impl_->Stop(); }
PipelineState LivePipeline::state() const noexcept { return impl_->state(); }
PipelineStatus LivePipeline::status() const { return impl_->status(); }
LivePipelineReadyInfo LivePipeline::ready_info() const {
  return impl_->ready_info();
}

}  // namespace mw::streamer

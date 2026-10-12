#include "mw/streamer/pipeline/remux_pipeline.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

bool Terminal(PipelineState state) {
  return state == PipelineState::kStopped || state == PipelineState::kEnded ||
         state == PipelineState::kFailed;
}

FfmpegInputConfig InputConfig(const RemuxPipelineConfig& config) {
  if (config.input.mode != InputMode::kRemux) {
    throw std::invalid_argument("RemuxPipeline 需要 kRemux 输入模式");
  }
  return config.input;
}

template <typename Callback, typename... Args>
void Notify(const Callback& callback, Args&&... args) noexcept {
  if (!callback) return;
  try {
    callback(std::forward<Args>(args)...);
  } catch (const std::exception& error) {
    MW_LOG_ERROR("streamer", "RemuxPipeline 业务回调异常: {}", error.what());
  } catch (...) {
    MW_LOG_ERROR("streamer", "RemuxPipeline 业务回调发生未知异常");
  }
}

}  // namespace

RemuxPipelineConfig::RemuxPipelineConfig() { input.mode = InputMode::kRemux; }

class RemuxPipeline::Impl final {
 public:
  explicit Impl(RemuxPipelineConfig config)
      : config_(std::move(config)),
        input_(InputConfig(config_)),
        remuxer_(config_.remuxer) {
    input_.SetOnReady([this](const auto& streams) { Prepare(streams); });
    input_.SetOnPacket([this](auto generation, const auto& packet) {
      Submit(generation, packet);
    });
    input_.SetOnStateChanged([this](auto state, int code, auto message) {
      InputChanged(state, code, message);
    });
    remuxer_.SetOnError([this](auto target, int code, auto message) {
      if (!target.empty()) {
        MW_LOG_WARNING("streamer",
                       "RemuxPipeline 输出错误: target={} code={} {}", target,
                       code, message);
        return;
      }
      Fail("remuxer", code, message);
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
            StartupFailed("input", std::current_exception());
            throw;
          }
        });
  }

  void SetOnReady(OnReady callback) {
    std::lock_guard<std::mutex> control(control_);
    RequireConfigurable();
    on_ready_ = std::move(callback);
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
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("RemuxPipeline 输入地址不能为空");
    }
    {
      std::lock_guard<std::mutex> control(control_);
      const auto previous = state();
      if (previous != PipelineState::kIdle && !Terminal(previous)) {
        throw std::logic_error("RemuxPipeline 已经启动");
      }
      if (worker_.joinable()) worker_.join();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.state = previous;
        ready_info_ = {};
        ready_ = configured_ = finish_requested_ = false;
        finish_state_ = PipelineState::kStopped;
        startup_error_ = nullptr;
      }
      ChangeState(PipelineState::kStarting);
      try {
        worker_ = std::thread([this] { Run(); });
      } catch (...) {
        StartupFailed("pipeline", std::current_exception());
        StopRemuxer();
        ChangeState(PipelineState::kFailed);
        throw;
      }
      try {
        input_.Start(url);
      } catch (...) {
        StartupFailed("input", std::current_exception());
      }
    }
    std::exception_ptr error;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return ready_ || Terminal(status_.state); });
      if (ready_ || (configured_ && status_.state == PipelineState::kEnded)) {
        return;
      }
      error = startup_error_;
    }
    Stop();
    if (error) std::rethrow_exception(error);
    throw ffmpeg::FfmpegException(AVERROR_EXIT, "RemuxPipeline 启动被停止");
  }

  void Stop() {
    std::lock_guard<std::mutex> control(control_);
    if (!worker_.joinable()) {
      StopRemuxer();
      if (state() == PipelineState::kIdle) ChangeState(PipelineState::kStopped);
      return;
    }
    Finish(PipelineState::kStopped);
    worker_.join();
    StopRemuxer();
  }

  PipelineState state() const noexcept { return state_.load(); }

  PipelineStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }

  RemuxPipelineReadyInfo ready_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_info_;
  }

 private:
  void RequireAdding() const {
    if (!Terminal(state()) && Finishing()) {
      throw std::logic_error("RemuxPipeline 停止过程中不能添加输出");
    }
  }

  void UpdateOutputs() {
    auto info = remuxer_.output_info();
    std::lock_guard<std::mutex> lock(mutex_);
    ready_info_.output_urls = std::move(info.output_urls);
    ready_info_.rtsp_url = std::move(info.rtsp_url);
    ready_info_.hls_url = std::move(info.hls_url);
  }

  void StopRemuxer() {
    std::lock_guard<std::mutex> outputs(outputs_);
    remuxer_.Stop();
  }

  void RequireConfigurable() {
    if (worker_.joinable())
      throw std::logic_error("配置 RemuxPipeline 回调前需要 Stop");
  }

  bool Finishing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_requested_;
  }

  void ChangeState(PipelineState next, bool input_event = false) {
    // Preserve notification order when input and cleanup threads transition.
    std::lock_guard<std::mutex> notification(notifications_);
    PipelineStatus snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if ((input_event && finish_requested_) || status_.state == next) return;
      status_.state = next;
      state_.store(next);
      snapshot = status_;
    }
    Notify(on_state_changed_, snapshot);
    changed_.notify_all();
  }

  void Finish(PipelineState result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finish_requested_ || Terminal(status_.state)) return;
    finish_requested_ = true;
    finish_state_ = result;
    changed_.notify_all();
  }

  void StartupFailed(std::string_view source, std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const ffmpeg::FfmpegException& failure) {
      Fail(source, failure.error_code(), failure.what(), error);
    } catch (const std::exception& failure) {
      Fail(source, AVERROR_EXTERNAL, failure.what(), error);
    } catch (...) {
      Fail(source, AVERROR_UNKNOWN, "管线初始化发生未知异常", error);
    }
  }

  void Fail(std::string_view source, int code, std::string_view message,
            std::exception_ptr error = {}) {
    MW_LOG_ERROR("streamer", "RemuxPipeline 会话失败: source={} code={} {}",
                 source, code, message);
    std::lock_guard<std::mutex> lock(mutex_);
    if (Terminal(status_.state)) return;
    if (status_.error_source.empty()) {
      status_.error_source = source;
      status_.error_code = code;
      status_.error_message = message;
      if (!ready_ && !startup_error_) {
        startup_error_ = error ? std::move(error)
                               : std::make_exception_ptr(
                                     ffmpeg::FfmpegException(code, message));
      }
    }
    finish_requested_ = true;
    finish_state_ = PipelineState::kFailed;
    changed_.notify_all();
  }

  void Prepare(const std::vector<ffmpeg::StreamInfo>& streams) {
    try {
      std::unique_lock<std::mutex> outputs(outputs_);
      if (Finishing()) return;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configured_) {
          if (ready_info_.streams != streams) {
            throw std::invalid_argument(
                "RemuxPipeline 输出不支持运行中改变轨道配置，请重新启动管线");
          }
          return;
        }
      }
      remuxer_.Start(streams);
      RemuxPipelineReadyInfo info;
      info.streams = streams;
      auto output_info = remuxer_.output_info();
      info.output_urls = std::move(output_info.output_urls);
      info.rtsp_url = std::move(output_info.rtsp_url);
      info.hls_url = std::move(output_info.hls_url);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_info_ = info;
        configured_ = true;
      }
      outputs.unlock();
      Notify(on_ready_, info);
    } catch (...) {
      StartupFailed("remuxer", std::current_exception());
    }
  }

  void Submit(std::uint64_t generation, const ffmpeg::Packet& packet) {
    if (Finishing()) return;
    try {
      if (!remuxer_.SubmitPacket(generation, packet)) {
        // The worker reports the specific session failure through OnError.
        Finish(PipelineState::kFailed);
      }
    } catch (const ffmpeg::FfmpegException& error) {
      Fail("remuxer", error.error_code(), error.what(),
           std::current_exception());
    } catch (const std::exception& error) {
      Fail("remuxer", AVERROR_EXTERNAL, error.what(), std::current_exception());
    } catch (...) {
      Fail("remuxer", AVERROR_UNKNOWN, "原包提交发生未知异常",
           std::current_exception());
    }
  }

  void InputChanged(InputState state, int code, std::string_view message) {
    switch (state) {
      case InputState::kConnected: {
        ChangeState(PipelineState::kRunning, true);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!finish_requested_) ready_ = true;
        changed_.notify_all();
        break;
      }
      case InputState::kWaitingRetry:
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
      // Keep consumption alive until Input's in-flight submission returns.
      input_.Stop();
      StopRemuxer();
    } catch (const std::exception& error) {
      StopRemuxer();
      Fail("pipeline", AVERROR_UNKNOWN, error.what(), std::current_exception());
    } catch (...) {
      StopRemuxer();
      Fail("pipeline", AVERROR_UNKNOWN, "管线停止发生未知异常",
           std::current_exception());
    }
    PipelineState result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result = finish_state_;
    }
    ChangeState(result);
  }

  const RemuxPipelineConfig config_;
  FfmpegInput input_;
  SyncRemuxer remuxer_;
  std::mutex control_;
  std::mutex outputs_;
  std::mutex notifications_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::atomic<PipelineState> state_{PipelineState::kIdle};
  PipelineStatus status_;
  RemuxPipelineReadyInfo ready_info_;
  bool configured_ = false;
  bool ready_ = false;
  bool finish_requested_ = false;
  PipelineState finish_state_ = PipelineState::kStopped;
  std::exception_ptr startup_error_;
  OnReady on_ready_;
  OnStateChanged on_state_changed_;
};

RemuxPipeline::RemuxPipeline(RemuxPipelineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
RemuxPipeline::~RemuxPipeline() = default;
void RemuxPipeline::SetOnSelectStreams(OnSelectStreams callback) {
  impl_->SetOnSelectStreams(std::move(callback));
}
void RemuxPipeline::SetOnReady(OnReady callback) {
  impl_->SetOnReady(std::move(callback));
}
void RemuxPipeline::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
std::string RemuxPipeline::AddPushUrl(const std::string& url) {
  return impl_->AddPushUrl(url);
}
void RemuxPipeline::AddRtspPublish(const std::string& app,
                                   const std::string& stream,
                                   const std::string& ip, std::uint16_t port,
                                   bool tls) {
  impl_->AddRtspPublish(app, stream, ip, port, tls);
}
std::string RemuxPipeline::AddHlsPublish(const std::string& app,
                                         const std::string& stream,
                                         const std::string& ip,
                                         std::uint16_t port, bool tls) {
  return impl_->AddHlsPublish(app, stream, ip, port, tls);
}
void RemuxPipeline::Start(std::string_view url) { impl_->Start(url); }
void RemuxPipeline::Stop() { impl_->Stop(); }
PipelineState RemuxPipeline::state() const noexcept { return impl_->state(); }
PipelineStatus RemuxPipeline::status() const { return impl_->status(); }
RemuxPipelineReadyInfo RemuxPipeline::ready_info() const {
  return impl_->ready_info();
}

}  // namespace mw::streamer

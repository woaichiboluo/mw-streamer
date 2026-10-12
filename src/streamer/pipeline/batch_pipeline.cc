#include "mw/streamer/pipeline/batch_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
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

FfmpegInputConfig InputConfig(const BatchPipelineConfig& config) {
  if (config.input.mode != InputMode::kBatch) {
    throw std::invalid_argument("BatchPipeline 需要 kBatch 输入模式");
  }
  if (!std::isfinite(config.playback_speed) || config.playback_speed < 0.5 ||
      config.playback_speed > 8.0) {
    throw std::invalid_argument(
        "BatchPipeline playback_speed 必须在 [0.5, 8.0]");
  }
  return config.input;
}

}  // namespace

BatchPipelineConfig::BatchPipelineConfig() { input.mode = InputMode::kBatch; }

class BatchPipeline::Impl final {
 public:
  Impl(const ffmpeg::HwDeviceContext& video_device, BatchPipelineConfig config)
      : video_device_(video_device),
        input_(video_device_, InputConfig(config)),
        playback_speed_(config.playback_speed) {
    input_.SetOnReady([this](const auto& streams) { Prepare(streams); });
    input_.SetOnPacket([this](auto generation, const auto&) {
      if (!generation_ || *generation_ != generation) {
        generation_ = generation;
        for (auto& [index, clock] : delivery_clocks_) {
          clock.anchor.reset();
          clock.ended = false;
        }
      }
    });
    input_.SetOnFrame(
        [this](int index, const auto& frame) { Consume(index, frame); });
    input_.SetOnTrackEnded(
        [this](int index, auto end_pts) { EndTrack(index, end_pts); });
    input_.SetOnStateChanged([this](auto state, int code, auto message) {
      InputChanged(state, code, message);
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

  void Start(std::string_view url) {
    if (url.empty() ||
        url.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
      throw std::invalid_argument("BatchPipeline 输入地址不能为空");
    }
    {
      std::lock_guard<std::mutex> control(control_);
      const auto previous = state();
      if (previous != PipelineState::kIdle && !Terminal(previous)) {
        throw std::logic_error("BatchPipeline 已经启动");
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
        generation_.reset();
        delivery_clocks_.clear();
      }
      ChangeState(PipelineState::kStarting);
      try {
        worker_ = std::thread([this] { Run(); });
      } catch (...) {
        StartupFailed("pipeline", std::current_exception());
        processor_.Stop();
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
    throw ffmpeg::FfmpegException(AVERROR_EXIT, "BatchPipeline 启动被停止");
  }

  void Stop() {
    std::lock_guard<std::mutex> control(control_);
    if (!worker_.joinable()) {
      processor_.Stop();
      if (state() == PipelineState::kIdle) ChangeState(PipelineState::kStopped);
      return;
    }
    Finish(PipelineState::kStopped);
    worker_.join();
    processor_.Stop();
  }

  PipelineState state() const noexcept { return state_.load(); }
  PipelineStatus status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
  }
  BatchPipelineReadyInfo ready_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_info_;
  }

 private:
  void RequireConfigurable() {
    if (worker_.joinable()) {
      throw std::logic_error("配置 BatchPipeline 回调前需要 Stop");
    }
  }
  bool Finishing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return finish_requested_;
  }

  void ChangeState(PipelineState next, bool input_event = false) {
    std::lock_guard<std::mutex> notification(notifications_);
    PipelineStatus snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if ((input_event && finish_requested_) || status_.state == next) return;
      status_.state = next;
      state_.store(next);
      snapshot = status_;
    }
    if (on_state_changed_) {
      try {
        on_state_changed_(snapshot);
      } catch (const std::exception& error) {
        MW_LOG_ERROR("streamer", "BatchPipeline 状态回调异常: {}",
                     error.what());
      } catch (...) {
        MW_LOG_ERROR("streamer", "BatchPipeline 状态回调发生未知异常");
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
    MW_LOG_ERROR("streamer", "BatchPipeline 会话失败: source={} code={} {}",
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
      if (Finishing()) return;
      if (!processor_.Start(streams, video_device_)) {
        throw std::runtime_error("BatchProcessor 启动被 OnReady 拒绝");
      }
      std::lock_guard<std::mutex> lock(mutex_);
      ready_info_.streams = streams;
      delivery_clocks_.clear();
      for (const auto& stream : streams) {
        delivery_clocks_.emplace(stream.stream_index,
                                 DeliveryClock{stream.time_base});
      }
      configured_ = true;
    } catch (...) {
      StartupFailed("processor", std::current_exception());
    }
  }

  using Clock = std::chrono::steady_clock;
  struct DeliveryClock {
    AVRational time_base;
    std::optional<long double> anchor;
    Clock::time_point started;
    std::int64_t end_pts = AV_NOPTS_VALUE;
    bool ended = false;
  };

  Clock::time_point Deadline(const DeliveryClock& clock, std::int64_t pts,
                             AVRational time_base) const {
    if (!clock.anchor || pts == AV_NOPTS_VALUE || time_base.num <= 0 ||
        time_base.den <= 0) {
      return Clock::time_point::min();
    }
    const long double media_seconds =
        static_cast<long double>(pts) * time_base.num / time_base.den;
    const long double seconds =
        (media_seconds - *clock.anchor) / playback_speed_;
    if (seconds <= 0) return clock.started;
    const auto available = Clock::time_point::max() - clock.started;
    const auto delay =
        seconds >= std::chrono::duration<long double>(available).count()
            ? available
            : std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<long double>(seconds));
    return clock.started + delay;
  }

  bool WaitForDelivery(int stream_index, const ffmpeg::Frame& frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (finish_requested_) return false;
    if (frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      return true;
    }
    const long double media_seconds = static_cast<long double>(frame->pts) *
                                      frame->time_base.num /
                                      frame->time_base.den;
    auto& clock = delivery_clocks_.at(stream_index);
    if (!clock.anchor) {
      clock.anchor = media_seconds;
      clock.started = Clock::now();
    }
    return !changed_.wait_until(lock,
                                Deadline(clock, frame->pts, frame->time_base),
                                [this] { return finish_requested_; });
  }

  void EndTrack(int stream_index, std::int64_t end_pts) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (finish_requested_) return;
    const auto track = delivery_clocks_.find(stream_index);
    if (track == delivery_clocks_.end()) return;
    auto& ended = track->second;
    ended.end_pts = end_pts;
    ended.ended = true;
    auto due = Clock::time_point::min();
    for (const auto& [index, clock] : delivery_clocks_) {
      if (!clock.ended) return;
      due = std::max(due, Deadline(clock, clock.end_pts, clock.time_base));
    }
    // Preserve the final frame's duration before EOF or the next loop. Waiting
    // only at frame PTS would make a one-frame loop run without a rate limit.
    changed_.wait_until(lock, due, [this] { return finish_requested_; });
  }

  void Consume(int stream_index, const ffmpeg::Frame& frame) {
    if (!WaitForDelivery(stream_index, frame)) return;
    if (frame->nb_samples > 0) {
      processor_.ProcessAudio(frame);
    } else {
      processor_.ProcessVideo(frame);
    }
    // Input's decoded kConnected event precedes its first frame. Publish
    // running and finish synchronous Start only after the consumer has
    // returned.
    if (state() != PipelineState::kRunning) {
      ChangeState(PipelineState::kRunning, true);
      std::lock_guard<std::mutex> lock(mutex_);
      if (!finish_requested_) ready_ = true;
      changed_.notify_all();
    }
  }

  void InputChanged(InputState state, int code, std::string_view message) {
    switch (state) {
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
      input_.Stop();
      PipelineState result;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        result = finish_state_;
      }
      if (result == PipelineState::kEnded) processor_.End();
      processor_.Stop();
    } catch (const std::exception& error) {
      processor_.Stop();
      Fail("pipeline", AVERROR_UNKNOWN, error.what(), std::current_exception());
    } catch (...) {
      processor_.Stop();
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

  ffmpeg::HwDeviceContext video_device_;
  FfmpegInput input_;
  const double playback_speed_;
  // Updated only by the serialized input callbacks, or before a new Start.
  std::optional<std::uint64_t> generation_;
  std::map<int, DeliveryClock> delivery_clocks_;
  BatchProcessor processor_;
  std::mutex control_;
  std::mutex notifications_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::atomic<PipelineState> state_{PipelineState::kIdle};
  PipelineStatus status_;
  BatchPipelineReadyInfo ready_info_;
  bool configured_ = false;
  bool ready_ = false;
  bool finish_requested_ = false;
  PipelineState finish_state_ = PipelineState::kStopped;
  std::exception_ptr startup_error_;
  OnStateChanged on_state_changed_;
};

BatchPipeline::BatchPipeline(BatchPipelineConfig config)
    : BatchPipeline(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCpu),
                    std::move(config)) {}
BatchPipeline::BatchPipeline(const ffmpeg::HwDeviceContext& video_device,
                             BatchPipelineConfig config)
    : impl_(std::make_unique<Impl>(video_device, std::move(config))) {}
BatchPipeline::~BatchPipeline() = default;
void BatchPipeline::SetOnSelectStreams(OnSelectStreams callback) {
  impl_->SetOnSelectStreams(std::move(callback));
}
void BatchPipeline::SetOnReady(OnReady callback) {
  impl_->SetOnReady(std::move(callback));
}
void BatchPipeline::SetOnVideo(OnVideo callback) {
  impl_->SetOnVideo(std::move(callback));
}
void BatchPipeline::SetOnAudio(OnAudio callback) {
  impl_->SetOnAudio(std::move(callback));
}
void BatchPipeline::SetOnEnded(OnEnded callback) {
  impl_->SetOnEnded(std::move(callback));
}
void BatchPipeline::SetOnStop(OnStop callback) {
  impl_->SetOnStop(std::move(callback));
}
void BatchPipeline::SetOnStateChanged(OnStateChanged callback) {
  impl_->SetOnStateChanged(std::move(callback));
}
void BatchPipeline::Start(std::string_view url) { impl_->Start(url); }
void BatchPipeline::Stop() { impl_->Stop(); }
PipelineState BatchPipeline::state() const noexcept { return impl_->state(); }
PipelineStatus BatchPipeline::status() const { return impl_->status(); }
BatchPipelineReadyInfo BatchPipeline::ready_info() const {
  return impl_->ready_info();
}

}  // namespace mw::streamer

#include "mw/streamer/filter/av_media_filter.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <ffnvcodec/dynlink_cuda.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/log.h"
#include "mw/streamer/performance/latency.h"

namespace mw::streamer {
namespace {

// Load lazily: CPU and audio callbacks must work without an NVIDIA driver.
class CudaDriver final {
 public:
  CudaDriver() {
#if defined(_WIN32)
    library_ = LoadLibraryW(L"nvcuda.dll");
#else
    library_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!library_) throw std::runtime_error("无法加载滤镜 CUDA 驱动");
    try {
      push = Load<tcuCtxPushCurrent_v2*>("cuCtxPushCurrent_v2");
      pop = Load<tcuCtxPopCurrent_v2*>("cuCtxPopCurrent_v2");
    } catch (...) {
      Close();
      throw;
    }
  }
  ~CudaDriver() { Close(); }
  CudaDriver(const CudaDriver&) = delete;
  CudaDriver& operator=(const CudaDriver&) = delete;

  tcuCtxPushCurrent_v2* push = nullptr;
  tcuCtxPopCurrent_v2* pop = nullptr;

 private:
  template <typename Function>
  Function Load(const char* name) {
#if defined(_WIN32)
    const auto symbol = GetProcAddress(library_, name);
#else
    const auto symbol = dlsym(library_, name);
#endif
    if (!symbol) throw std::runtime_error("CUDA 驱动缺少滤镜上下文接口");
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
  }

  void Close() noexcept {
#if defined(_WIN32)
    FreeLibrary(library_);
#else
    dlclose(library_);
#endif
  }

#if defined(_WIN32)
  HMODULE library_ = nullptr;
#else
  void* library_ = nullptr;
#endif
};

class VideoContextScope final {
 public:
  VideoContextScope(const AVFrame& frame, bool video) {
    if (!video || frame.format != AV_PIX_FMT_CUDA) return;
    if (!frame.hw_frames_ctx || !frame.hw_frames_ctx->data) {
      throw std::invalid_argument("CUDA 滤镜帧缺少硬件帧上下文");
    }
    const auto* frames =
        reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
    if (!frames->device_ctx ||
        frames->device_ctx->type != AV_HWDEVICE_TYPE_CUDA ||
        !frames->device_ctx->hwctx) {
      throw std::invalid_argument("CUDA 滤镜帧缺少 CUDA 设备上下文");
    }
    const auto* context =
        static_cast<const AVCUDADeviceContext*>(frames->device_ctx->hwctx);
    if (!context->cuda_ctx) {
      throw std::invalid_argument("CUDA 滤镜设备上下文无效");
    }
    static const CudaDriver driver;
    if (driver.push(context->cuda_ctx) != CUDA_SUCCESS) {
      throw std::runtime_error("无法绑定滤镜 CUDA 上下文");
    }
    driver_ = &driver;
  }

  ~VideoContextScope() {
    if (driver_ && Pop() != CUDA_SUCCESS) {
      MW_LOG_ERROR("streamer", "异常退出滤镜时恢复 CUDA 上下文失败");
    }
  }
  VideoContextScope(const VideoContextScope&) = delete;
  VideoContextScope& operator=(const VideoContextScope&) = delete;

  void Finish() {
    if (driver_ && Pop() != CUDA_SUCCESS) {
      throw std::runtime_error("恢复滤镜 CUDA 上下文失败");
    }
  }

 private:
  CUresult Pop() noexcept {
    CUcontext previous = nullptr;
    const auto result = driver_->pop(&previous);
    driver_ = nullptr;
    return result;
  }

  const CudaDriver* driver_ = nullptr;
};

void LogFailure(const void* instance, const char* operation,
                std::exception_ptr error, int stream_index = -1) noexcept {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& failure) {
    MW_LOG_ERROR("streamer",
                 "AvMediaFilter[{}] 回调失败: operation={} stream={} error={}",
                 instance, operation, stream_index, failure.what());
  } catch (...) {
    MW_LOG_ERROR("streamer",
                 "AvMediaFilter[{}] 回调失败: operation={} stream={} 未知异常",
                 instance, operation, stream_index);
  }
}

}  // namespace

class AvMediaFilter::Impl final {
 public:
  ~Impl() {
    try {
      Stop();
    } catch (const std::exception& error) {
      MW_LOG_ERROR("streamer", "AvMediaFilter 停止回调失败: {}", error.what());
    } catch (...) {
      MW_LOG_ERROR("streamer", "AvMediaFilter 停止回调发生未知异常");
    }
  }

  template <typename Callback>
  void Configure(Callback& target, Callback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("配置 AvMediaFilter 回调前需要 Stop");
    }
    target = std::move(callback);
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("AvMediaFilter 重新启动前需要 Stop");
    }
    if (on_start_) on_start_();
    video_latency_.Start(PerformanceStage::kMediaFilterVideo, this);
    audio_latency_.Start(PerformanceStage::kMediaFilterAudio, this);
    error_ = {};
    started_ = true;
    MW_LOG_INFO("streamer", "AvMediaFilter 已启动: video={}, audio={}",
                static_cast<bool>(on_video_), static_cast<bool>(on_audio_));
  }

  void Stop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) return;
    if (stopping_) {
      idle_.wait(lock, [this] { return !stopping_; });
      return;
    }
    stopping_ = true;
    MW_LOG_DEBUG("streamer", "AvMediaFilter[{}] 等待处理结束: active={}",
                 fmt::ptr(this), active_calls_);
    idle_.wait(lock, [this] { return active_calls_ == 0; });
    video_latency_.Finish();
    audio_latency_.Finish();
    started_ = false;
    stopping_ = false;
    idle_.notify_all();
    MW_LOG_INFO("streamer", "AvMediaFilter 已停止");
    if (on_stop_) on_stop_();
  }

  std::exception_ptr error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  void Reset() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_ || stopping_ || error_) {
      throw std::logic_error("AvMediaFilter 重置前需要成功 Start");
    }
    MW_LOG_DEBUG("streamer", "AvMediaFilter[{}] 开始重置: active={}",
                 fmt::ptr(this), active_calls_);
    idle_.wait(lock, [this] { return active_calls_ == 0; });
    if (error_) {
      throw std::logic_error("AvMediaFilter 回调失败后需要 Stop/Start");
    }
    try {
      if (on_reset_) on_reset_();
    } catch (...) {
      error_ = std::current_exception();
      LogFailure(this, "reset", error_);
      throw;
    }
    MW_LOG_DEBUG("streamer", "AvMediaFilter[{}] 重置完成", fmt::ptr(this));
  }

  AvDecoderReadResult Process(AvDecoderReadResult input, bool video,
                              bool consume = false) {
    const auto& callback = video ? on_video_ : on_audio_;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_ || stopping_ || error_) {
        throw std::logic_error("AvMediaFilter 处理帧前需要成功 Start");
      }
      if (input.status != AvDecoderReadStatus::kFrame) {
        if (input.status == AvDecoderReadStatus::kEof)
          MW_LOG_DEBUG("streamer",
                       "AvMediaFilter[{}] 透传 EOF: track={} stream={} "
                       "end_pts={} time_base={}/{}",
                       fmt::ptr(this), video ? "video" : "audio",
                       input.stream_index, input.end_pts, input.time_base.num,
                       input.time_base.den);
        return input;
      }
      if (!input.frame || !input.frame->get()) {
        throw std::invalid_argument("AvMediaFilter 缺少输入帧");
      }
      if (!callback) {
        MW_LOG_TRACE("streamer",
                     "AvMediaFilter[{}] 无业务回调: track={} stream={} pts={} "
                     "consume={}",
                     fmt::ptr(this), video ? "video" : "audio",
                     input.stream_index, input.frame->get()->pts, consume);
        return input;
      }
      ++active_calls_;
    }
    const auto* source = input.frame->get();
    const auto pts = source->pts;
    const auto dts = source->pkt_dts;
    const auto duration = source->duration;
    MW_LOG_TRACE("streamer",
                 "AvMediaFilter[{}] 开始处理帧: track={} stream={} pts={} "
                 "dts={} duration={} time_base={}/{} consume={}",
                 fmt::ptr(this), video ? "video" : "audio", input.stream_index,
                 pts, dts, duration, input.time_base.num, input.time_base.den,
                 consume);
    auto& latency = video ? video_latency_ : audio_latency_;
    const auto started = latency.Begin();
    std::optional<std::int64_t> elapsed;
    try {
      VideoContextScope context(*source, video);
      auto output = callback(*input.frame);
      context.Finish();
      if (latency.enabled()) {
        elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      internal::StageLatency::Clock::now() - started)
                      .count();
      }
      if (!consume) {
        if (output && !output->get()) {
          throw std::invalid_argument("AvMediaFilter 回调返回了无效帧");
        }
        if (output) {
          input.time_base = output->get()->time_base;
        } else {
          input.status = AvDecoderReadStatus::kNull;
        }
        input.frame = std::move(output);
      }
    } catch (...) {
      // Do not resume using partially-updated business
      // history. Stop still releases the successfully-started session.
      const auto error = std::current_exception();
      FinishProcessing(error);
      LogFailure(this, video ? "video" : "audio", error, input.stream_index);
      throw;
    }
    FinishProcessing({}, &latency, elapsed);
    MW_LOG_TRACE("streamer",
                 "AvMediaFilter[{}] 帧处理完成: track={} stream={} pts={} "
                 "consume={}",
                 fmt::ptr(this), video ? "video" : "audio", input.stream_index,
                 pts, consume);
    return input;
  }

  OnStart on_start_;
  OnStop on_stop_;
  OnReset on_reset_;
  OnAudio on_audio_;
  OnVideo on_video_;

 private:
  void FinishProcessing(std::exception_ptr error = {},
                        internal::StageLatency* latency = nullptr,
                        std::optional<std::int64_t> elapsed = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Callbacks, even on the same track, may overlap. Serialize accounting
    // only; the measured interval excludes this lock and metadata restoration.
    if (latency && elapsed) latency->Observe(elapsed);
    if (!error_) error_ = std::move(error);
    if (--active_calls_ == 0) idle_.notify_all();
  }

  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::size_t active_calls_ = 0;
  bool started_ = false;
  bool stopping_ = false;
  std::exception_ptr error_;
  internal::StageLatency video_latency_;
  internal::StageLatency audio_latency_;
};

AvMediaFilter::AvMediaFilter() : impl_(std::make_unique<Impl>()) {}
AvMediaFilter::~AvMediaFilter() = default;
void AvMediaFilter::SetOnStart(OnStart callback) {
  impl_->Configure(impl_->on_start_, std::move(callback));
}
void AvMediaFilter::SetOnStop(OnStop callback) {
  impl_->Configure(impl_->on_stop_, std::move(callback));
}
void AvMediaFilter::SetOnReset(OnReset callback) {
  impl_->Configure(impl_->on_reset_, std::move(callback));
}
void AvMediaFilter::SetOnAudio(OnAudio callback) {
  impl_->Configure(impl_->on_audio_, std::move(callback));
}
void AvMediaFilter::SetOnVideo(OnVideo callback) {
  impl_->Configure(impl_->on_video_, std::move(callback));
}
void AvMediaFilter::Start() { impl_->Start(); }
void AvMediaFilter::Stop() { impl_->Stop(); }
void AvMediaFilter::Reset() { impl_->Reset(); }
std::exception_ptr AvMediaFilter::error() const { return impl_->error(); }
AvDecoderReadResult AvMediaFilter::ProcessVideo(AvDecoderReadResult input) {
  return impl_->Process(std::move(input), true);
}
AvDecoderReadResult AvMediaFilter::ProcessAudio(AvDecoderReadResult input) {
  return impl_->Process(std::move(input), false);
}
void AvMediaFilter::ConsumeVideo(AvDecoderReadResult input) {
  impl_->Process(std::move(input), true, true);
}
void AvMediaFilter::ConsumeAudio(AvDecoderReadResult input) {
  impl_->Process(std::move(input), false, true);
}

}  // namespace mw::streamer

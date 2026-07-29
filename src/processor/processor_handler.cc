#include "mw/processor/processor_handler.h"

#include <cuda.h>
#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/processor/frame_adapter.h"

namespace mw::streamer::processor {
namespace {

constexpr int kProcessorAudioSampleRate = 48000;

enum class HandlerState {
  kReady,
  kStarted,
  kStopped,
};

std::string CudaErrorText(CUresult result) {
  const char* name = nullptr;
  const char* description = nullptr;
  cuGetErrorName(result, &name);
  cuGetErrorString(result, &description);
  return fmt::format("{}: {}", name ? name : "CUDA_ERROR_UNKNOWN",
                     description ? description : "未知CUDA错误");
}

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result != CUDA_SUCCESS) {
    throw std::runtime_error(
        fmt::format("{}失败: {}", operation, CudaErrorText(result)));
  }
}

const AVHWFramesContext& GetCudaFramesContext(const AVFrame& frame) {
  if (frame.format != AV_PIX_FMT_CUDA || !frame.hw_frames_ctx ||
      !frame.hw_frames_ctx->data) {
    throw std::invalid_argument("CUDA视频帧缺少硬件帧上下文");
  }

  const auto* frames_context =
      reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
  if (frames_context->format != AV_PIX_FMT_CUDA ||
      frames_context->sw_format == AV_PIX_FMT_NONE ||
      !frames_context->device_ref || !frames_context->device_ctx ||
      frames_context->device_ctx->type != AV_HWDEVICE_TYPE_CUDA) {
    throw std::invalid_argument("视频帧不是有效的CUDA硬件帧");
  }
  return *frames_context;
}

const AVCUDADeviceContext& GetCudaDeviceContext(
    const AVHWFramesContext& frames_context) {
  const auto* cuda_context =
      static_cast<const AVCUDADeviceContext*>(frames_context.device_ctx->hwctx);
  if (!cuda_context || !cuda_context->cuda_ctx || !cuda_context->stream) {
    throw std::invalid_argument("CUDA视频帧缺少有效的设备上下文");
  }
  return *cuda_context;
}

void ValidateSourceInfo(const MwStreamerProcessorSourceInfo& source_info) {
  if (!source_info.has_video && !source_info.has_audio) {
    throw std::invalid_argument("Processor至少需要一个音频流或视频流");
  }
  if (source_info.has_video &&
      (source_info.video.width == 0 || source_info.video.height == 0)) {
    throw std::invalid_argument("Processor视频源缺少有效的宽高");
  }
  if (source_info.has_audio && (source_info.audio.sample_rate == 0 ||
                                source_info.audio.channel_count == 0)) {
    throw std::invalid_argument("Processor音频源缺少有效的采样率或声道数");
  }
}

void ValidateExecution(const MwStreamerExecutionContext& execution) {
  switch (execution.type) {
    case kMwStreamerExecutionCpu:
      if (execution.native_handle != 0) {
        throw std::invalid_argument("CPU执行上下文不能包含原生句柄");
      }
      return;
    case kMwStreamerExecutionCuda:
      if (execution.native_handle == 0) {
        throw std::invalid_argument("CUDA执行上下文缺少Stream句柄");
      }
      return;
    default:
      throw std::invalid_argument("Processor包含未知的执行上下文类型");
  }
}

void ValidateMode(MwStreamerProcessorMode mode) {
  switch (mode) {
    case kStreaming:
    case kLocalFile:
      return;
    default:
      throw std::invalid_argument("Processor包含未知的运行模式");
  }
}

void ValidateBoundaryReason(MwStreamerProcessorBoundaryReason reason) {
  switch (reason) {
    case kMwStreamerProcessorTimelineReset:
    case kMwStreamerProcessorEndOfInput:
      return;
    default:
      throw std::invalid_argument("Processor包含未知的输入边界原因");
  }
}

void CopyFrameProperties(const AVFrame& input, AVFrame* output) {
  ffmpeg::ThrowIfError(av_frame_copy_props(output, &input),
                       "复制Processor输出帧属性");
  output->crop_top = 0;
  output->crop_bottom = 0;
  output->crop_left = 0;
  output->crop_right = 0;
}

void FillBlack(AVFrame* frame, AVPixelFormat format, AVColorRange range) {
  std::array<std::ptrdiff_t, 4> linesizes{};
  for (std::size_t index = 0; index < linesizes.size(); ++index) {
    linesizes[index] = frame->linesize[index];
  }
  ffmpeg::ThrowIfError(
      av_image_fill_black(frame->data, linesizes.data(), format, range,
                          frame->width, frame->height),
      "填充Processor默认黑帧");
}

}  // namespace

class ProcessorHandler::Impl final {
 public:
  Impl(MwStreamerProcessorMode mode,
       const MwStreamerProcessorSourceInfo& source_info,
       const MwStreamerExecutionContext& execution)
      : mode_(mode), source_info_(source_info), execution_(execution) {
    ValidateMode(mode_);
    ValidateSourceInfo(source_info_);
    ValidateExecution(execution_);
  }

  ~Impl() {
    Stop();
    av_buffer_unref(&output_frames_context_);
    av_buffer_unref(&input_device_context_);
    av_channel_layout_uninit(&audio_layout_);
  }

  void SetCallbacks(const MwStreamerProcessorCallbacks& callbacks) {
    RequireReady("设置Processor回调");
    callbacks_ = callbacks;
  }

  MwStreamerProcessorStartResult Start(
      const MwStreamerProcessorConfig& config) {
    RequireReady("启动Processor");
    ValidateConfig(config);

    output_width_ = config.output_width;
    output_height_ = config.output_height;
    config_ = config.config;

    const MwStreamerProcessorConfig owned_config = {
        output_width_,
        output_height_,
        config_.c_str(),
    };
    const MwStreamerProcessorStartRequest request = {
        mode_,
        &source_info_,
        &owned_config,
        &execution_,
    };
    const auto result =
        callbacks_.on_start
            ? callbacks_.on_start(&request, callbacks_.user_context)
            : kMwStreamerProcessorStartSuccess;
    if (result == kMwStreamerProcessorStartSuccess) {
      state_ = HandlerState::kStarted;
    }
    return result;
  }

  std::optional<ffmpeg::Frame> ProcessVideo(const ffmpeg::Frame& input) {
    RequireStarted("处理视频帧");
    if (!source_info_.has_video) {
      throw std::logic_error("Processor没有视频流");
    }

    const VideoFrameAdapter input_adapter(input);
    EnsureVideoInitialized(*input.get(), input_adapter.view());

    if (mode_ == kLocalFile) {
      if (callbacks_.process_video) {
        const MwStreamerVideoProcessRequest request = {
            &input_adapter.view(),
            nullptr,
            &execution_,
        };
        InvokeVideoCallback(*input.get(), request);
      }
      return std::nullopt;
    }

    ffmpeg::Frame output = callbacks_.process_video
                               ? AllocateVideoFrame(*input.get())
                               : DefaultVideoFrame(*input.get());
    if (callbacks_.process_video) {
      VideoBufferAdapter output_adapter(output);
      auto output_view = output_adapter.view();
      const MwStreamerVideoProcessRequest request = {
          &input_adapter.view(),
          &output_view,
          &execution_,
      };
      InvokeVideoCallback(*input.get(), request);
    }

    CopyFrameProperties(*input.get(), output.get());
    return output;
  }

  std::optional<ffmpeg::Frame> ProcessAudio(const ffmpeg::Frame& input) {
    RequireStarted("处理音频帧");
    if (!source_info_.has_audio) {
      throw std::logic_error("Processor没有音频流");
    }

    const AudioFrameAdapter input_adapter(input);
    EnsureAudioInitialized(*input.get());

    if (mode_ == kLocalFile) {
      if (callbacks_.process_audio) {
        const MwStreamerAudioProcessRequest request = {
            &input_adapter.view(),
            nullptr,
        };
        callbacks_.process_audio(&request, callbacks_.user_context);
      }
      return std::nullopt;
    }

    auto output = AllocateAudioFrame(*input.get());
    if (callbacks_.process_audio) {
      AudioBufferAdapter output_adapter(output);
      auto output_view = output_adapter.view();
      const MwStreamerAudioProcessRequest request = {
          &input_adapter.view(),
          &output_view,
      };
      callbacks_.process_audio(&request, callbacks_.user_context);
    } else {
      ffmpeg::ThrowIfError(av_frame_copy(output.get(), input.get()),
                           "复制Processor默认音频输出");
    }

    CopyFrameProperties(*input.get(), output.get());
    return output;
  }

  void NotifyBoundary(MwStreamerProcessorBoundaryReason reason) {
    RequireStarted("通知Processor输入边界");
    ValidateBoundaryReason(reason);
    if (callbacks_.on_boundary) {
      callbacks_.on_boundary(reason, callbacks_.user_context);
    }
  }

  void UpdateConfig(std::string config) {
    RequireStarted("更新Processor配置");
    config_ = std::move(config);
    if (callbacks_.update_config) {
      callbacks_.update_config(config_.c_str(), callbacks_.user_context);
    }
  }

  void Stop() {
    if (state_ != HandlerState::kStarted) {
      return;
    }
    state_ = HandlerState::kStopped;
    if (callbacks_.on_stop) {
      callbacks_.on_stop(callbacks_.user_context);
    }
  }

 private:
  void RequireReady(const char* operation) const {
    if (state_ != HandlerState::kReady) {
      throw std::logic_error(fmt::format("{}只能在启动前执行", operation));
    }
  }

  void RequireStarted(const char* operation) const {
    if (state_ != HandlerState::kStarted) {
      throw std::logic_error(fmt::format("{}只能在启动后执行", operation));
    }
  }

  void ValidateConfig(const MwStreamerProcessorConfig& config) const {
    if (!config.config) {
      throw std::invalid_argument("Processor配置字符串不能为空指针");
    }
    if (mode_ == kLocalFile) {
      if (config.output_width != 0 || config.output_height != 0) {
        throw std::invalid_argument("本地文件Processor不能配置媒体输出宽高");
      }
      return;
    }
    if (source_info_.has_video) {
      if (config.output_width == 0 || config.output_height == 0) {
        throw std::invalid_argument("Processor视频输出缺少有效的宽高");
      }
    } else if (config.output_width != 0 || config.output_height != 0) {
      throw std::invalid_argument("纯音频Processor不能配置视频输出宽高");
    }
  }

  void EnsureVideoInitialized(const AVFrame& input,
                              const MwStreamerVideoFrameView& view) {
    if (!video_initialized_) {
      if (execution_.type == kMwStreamerExecutionCpu) {
        if (view.buffer.memory_type != kMwStreamerMemoryHost) {
          throw std::invalid_argument("CPU Processor不能接受硬件视频帧");
        }
        input_video_format_ = static_cast<AVPixelFormat>(input.format);
      } else {
        if (view.buffer.memory_type != kMwStreamerMemoryCuda) {
          throw std::invalid_argument("CUDA Processor不能接受软件视频帧");
        }
        const auto& frames_context = GetCudaFramesContext(input);
        const auto& device_context = GetCudaDeviceContext(frames_context);
        if (reinterpret_cast<std::uintptr_t>(device_context.stream) !=
            execution_.native_handle) {
          throw std::invalid_argument(
              "Processor执行Stream与输入CUDA帧不属于同一执行上下文");
        }
        input_video_format_ = AV_PIX_FMT_CUDA;
        input_storage_format_ = frames_context.sw_format;
        auto* device_context_ref = av_buffer_ref(frames_context.device_ref);
        if (!device_context_ref) {
          throw std::bad_alloc();
        }
        try {
          if (mode_ == kStreaming) {
            CreateOutputFramesContext(frames_context);
          }
        } catch (...) {
          av_buffer_unref(&device_context_ref);
          throw;
        }
        input_device_context_ = device_context_ref;
      }
      input_video_width_ = input.width;
      input_video_height_ = input.height;
      video_initialized_ = true;
      return;
    }

    if (input.format != input_video_format_ ||
        input.width != input_video_width_ ||
        input.height != input_video_height_) {
      throw std::invalid_argument("当前链路不支持动态改变视频格式或分辨率");
    }
    if (input_video_format_ == AV_PIX_FMT_CUDA) {
      const auto& frames_context = GetCudaFramesContext(input);
      if (frames_context.sw_format != input_storage_format_ ||
          !input_device_context_ ||
          frames_context.device_ref->data != input_device_context_->data) {
        throw std::invalid_argument("当前链路不支持动态改变CUDA视频格式或设备");
      }
    }
  }

  void CreateOutputFramesContext(
      const AVHWFramesContext& input_frames_context) {
    output_frames_context_ =
        av_hwframe_ctx_alloc(input_frames_context.device_ref);
    if (!output_frames_context_) {
      throw std::bad_alloc();
    }

    auto* output_context =
        reinterpret_cast<AVHWFramesContext*>(output_frames_context_->data);
    output_context->format = AV_PIX_FMT_CUDA;
    output_context->sw_format = input_frames_context.sw_format;
    output_context->width = static_cast<int>(output_width_);
    output_context->height = static_cast<int>(output_height_);
    try {
      ffmpeg::ThrowIfError(av_hwframe_ctx_init(output_frames_context_),
                           "初始化Processor CUDA输出帧池");
    } catch (...) {
      av_buffer_unref(&output_frames_context_);
      throw;
    }
  }

  const AVHWFramesContext& OutputFramesContext() const {
    if (!output_frames_context_ || !output_frames_context_->data) {
      throw std::logic_error("Processor缺少CUDA输出帧池");
    }
    return *reinterpret_cast<const AVHWFramesContext*>(
        output_frames_context_->data);
  }

  ffmpeg::HardwareContext::CurrentScope MakeCudaContextCurrent(
      const AVFrame& input) const {
    const auto& frames_context = GetCudaFramesContext(input);
    return ffmpeg::HardwareContext::MakeCurrent(frames_context.device_ref);
  }

  void SynchronizeCuda() const {
    ThrowIfCudaError(cuStreamSynchronize(
                         reinterpret_cast<CUstream>(execution_.native_handle)),
                     "同步Processor CUDA输出");
  }

  void InvokeVideoCallback(const AVFrame& input,
                           const MwStreamerVideoProcessRequest& request) const {
    if (execution_.type == kMwStreamerExecutionCuda) {
      const auto current_scope = MakeCudaContextCurrent(input);
      callbacks_.process_video(&request, callbacks_.user_context);
      SynchronizeCuda();
      return;
    }
    callbacks_.process_video(&request, callbacks_.user_context);
  }

  ffmpeg::Frame AllocateVideoFrame(const AVFrame& input) const {
    ffmpeg::Frame output;
    if (input_video_format_ == AV_PIX_FMT_CUDA) {
      ffmpeg::ThrowIfError(
          av_hwframe_get_buffer(output_frames_context_, output.get(), 0),
          "分配Processor CUDA输出帧");
      return output;
    }

    output->format = input.format;
    output->width = static_cast<int>(output_width_);
    output->height = static_cast<int>(output_height_);
    ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 32),
                         "分配Processor软件视频输出帧");
    return output;
  }

  ffmpeg::Frame DefaultVideoFrame(const AVFrame& input) {
    if (!black_frame_ || black_frame_range_ != input.color_range) {
      black_frame_ = CreateBlackFrame(input);
      black_frame_range_ = input.color_range;
    }
    return black_frame_->Ref();
  }

  ffmpeg::Frame CreateBlackFrame(const AVFrame& input) {
    if (input_video_format_ != AV_PIX_FMT_CUDA) {
      auto black = AllocateVideoFrame(input);
      FillBlack(black.get(), input_video_format_, input.color_range);
      return black;
    }

    ffmpeg::Frame software_black;
    software_black->format = input_storage_format_;
    software_black->width = static_cast<int>(output_width_);
    software_black->height = static_cast<int>(output_height_);
    ffmpeg::ThrowIfError(av_frame_get_buffer(software_black.get(), 32),
                         "分配Processor CUDA黑帧暂存");
    FillBlack(software_black.get(), input_storage_format_, input.color_range);

    auto black = AllocateVideoFrame(input);
    const auto current_scope = MakeCudaContextCurrent(input);
    ffmpeg::ThrowIfError(
        av_hwframe_transfer_data(black.get(), software_black.get(), 0),
        "上传Processor CUDA默认黑帧");
    SynchronizeCuda();
    return black;
  }

  void EnsureAudioInitialized(const AVFrame& input) {
    if (input.ch_layout.nb_channels !=
        static_cast<int>(source_info_.audio.channel_count)) {
      throw std::invalid_argument("Processor音频帧声道数与源信息不一致");
    }
    if (!audio_initialized_) {
      ffmpeg::ThrowIfError(
          av_channel_layout_copy(&audio_layout_, &input.ch_layout),
          "保存Processor音频声道布局");
      audio_initialized_ = true;
      return;
    }
    if (av_channel_layout_compare(&audio_layout_, &input.ch_layout) != 0) {
      throw std::invalid_argument("当前链路不支持动态改变音频声道布局");
    }
  }

  ffmpeg::Frame AllocateAudioFrame(const AVFrame& input) const {
    ffmpeg::Frame output;
    output->format = AV_SAMPLE_FMT_FLT;
    output->sample_rate = kProcessorAudioSampleRate;
    output->nb_samples = input.nb_samples;
    ffmpeg::ThrowIfError(
        av_channel_layout_copy(&output->ch_layout, &input.ch_layout),
        "复制Processor输出音频声道布局");
    ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 0),
                         "分配Processor音频输出帧");
    return output;
  }

  MwStreamerProcessorMode mode_ = kStreaming;
  MwStreamerProcessorSourceInfo source_info_{};
  MwStreamerExecutionContext execution_{};
  MwStreamerProcessorCallbacks callbacks_{};
  HandlerState state_ = HandlerState::kReady;

  std::string config_;
  std::uint32_t output_width_ = 0;
  std::uint32_t output_height_ = 0;

  AVPixelFormat input_video_format_ = AV_PIX_FMT_NONE;
  AVPixelFormat input_storage_format_ = AV_PIX_FMT_NONE;
  int input_video_width_ = 0;
  int input_video_height_ = 0;
  AVBufferRef* input_device_context_ = nullptr;
  AVBufferRef* output_frames_context_ = nullptr;
  std::optional<ffmpeg::Frame> black_frame_;
  AVColorRange black_frame_range_ = AVCOL_RANGE_UNSPECIFIED;
  bool video_initialized_ = false;

  AVChannelLayout audio_layout_{};
  bool audio_initialized_ = false;
};

ProcessorHandler::ProcessorHandler(
    MwStreamerProcessorMode mode,
    const MwStreamerProcessorSourceInfo& source_info,
    const MwStreamerExecutionContext& execution)
    : impl_(std::make_unique<Impl>(mode, source_info, execution)) {}

ProcessorHandler::~ProcessorHandler() = default;

void ProcessorHandler::SetCallbacks(
    const MwStreamerProcessorCallbacks& callbacks) {
  impl_->SetCallbacks(callbacks);
}

MwStreamerProcessorStartResult ProcessorHandler::Start(
    const MwStreamerProcessorConfig& config) {
  return impl_->Start(config);
}

std::optional<ffmpeg::Frame> ProcessorHandler::ProcessVideo(
    const ffmpeg::Frame& input) {
  return impl_->ProcessVideo(input);
}

std::optional<ffmpeg::Frame> ProcessorHandler::ProcessAudio(
    const ffmpeg::Frame& input) {
  return impl_->ProcessAudio(input);
}

void ProcessorHandler::NotifyBoundary(
    MwStreamerProcessorBoundaryReason reason) {
  impl_->NotifyBoundary(reason);
}

void ProcessorHandler::UpdateConfig(std::string config) {
  impl_->UpdateConfig(std::move(config));
}

void ProcessorHandler::Stop() { impl_->Stop(); }

}  // namespace mw::streamer::processor

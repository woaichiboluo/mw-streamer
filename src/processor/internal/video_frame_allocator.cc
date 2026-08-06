#include "mw/processor/internal/video_frame_allocator.h"

#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"

namespace mw::streamer::processor::internal {
namespace {

const AVHWFramesContext& GetCudaFramesContext(const AVFrame& frame) {
  const auto* frames_context = ffmpeg::HardwareContext::GetFramesContext(frame);
  if (!frames_context || frames_context->format != AV_PIX_FMT_CUDA ||
      frames_context->device_ctx->type != AV_HWDEVICE_TYPE_CUDA) {
    throw std::invalid_argument("视频帧不是有效的CUDA硬件帧");
  }
  return *frames_context;
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

VideoFrameAllocator::VideoFrameAllocator(std::uint32_t output_width,
                                         std::uint32_t output_height)
    : output_width_(output_width), output_height_(output_height) {
  if (output_width_ == 0 || output_height_ == 0) {
    throw std::invalid_argument("视频分配器缺少有效的输出宽高");
  }
}

VideoFrameAllocator::~VideoFrameAllocator() {
  av_buffer_unref(&output_frames_context_);
  av_buffer_unref(&input_device_context_);
}

ffmpeg::Frame VideoFrameAllocator::Allocate(const ffmpeg::Frame& input) {
  if (!input.get()) {
    throw std::invalid_argument("不能根据空视频Frame分配输出");
  }
  PrepareOrValidate(*input.get());

  ffmpeg::Frame output;
  if (input_format_ == AV_PIX_FMT_CUDA) {
    ffmpeg::ThrowIfError(
        av_hwframe_get_buffer(output_frames_context_, output.get(), 0),
        "分配Processor CUDA输出帧");
    return output;
  }

  output->format = input_format_;
  output->width = static_cast<int>(output_width_);
  output->height = static_cast<int>(output_height_);
  ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 32),
                       "分配Processor软件视频输出帧");
  return output;
}

ffmpeg::Frame VideoFrameAllocator::GetBlackFrame(
    const ffmpeg::Frame& input,
    const ffmpeg::HardwareContext* hardware_context) {
  if (!input.get()) {
    throw std::invalid_argument("不能根据空视频Frame获取默认黑帧");
  }
  PrepareOrValidate(*input.get());
  if (black_frame_ && black_frame_->get()->color_range == input->color_range) {
    return black_frame_->Ref();
  }

  if (input_format_ != AV_PIX_FMT_CUDA) {
    black_frame_ = AllocateBlackFrame(input);
    return black_frame_->Ref();
  }
  if (!hardware_context || !hardware_context->IsCompatible(*input.get())) {
    throw std::invalid_argument("CUDA黑帧分配缺少兼容的硬件上下文");
  }

  black_frame_ = AllocateBlackFrame(input);
  return black_frame_->Ref();
}

ffmpeg::Frame VideoFrameAllocator::AllocateBlackFrame(
    const ffmpeg::Frame& input) {
  auto output = Allocate(input);
  output->color_range = input->color_range;
  if (input_format_ != AV_PIX_FMT_CUDA) {
    FillBlack(output.get(), input_format_, input->color_range);
    return output;
  }

  ffmpeg::Frame software_black;
  software_black->format = storage_format_;
  software_black->width = static_cast<int>(output_width_);
  software_black->height = static_cast<int>(output_height_);
  ffmpeg::ThrowIfError(av_frame_get_buffer(software_black.get(), 32),
                       "分配Processor CUDA黑帧暂存");
  FillBlack(software_black.get(), storage_format_, input->color_range);
  ffmpeg::ThrowIfError(
      av_hwframe_transfer_data(output.get(), software_black.get(), 0),
      "上传Processor CUDA默认黑帧");
  return output;
}

void VideoFrameAllocator::PrepareOrValidate(const AVFrame& input) {
  if (!prepared_) {
    Prepare(input);
    return;
  }
  ValidatePreparedInput(input);
}

void VideoFrameAllocator::Prepare(const AVFrame& input) {
  if (input.width <= 0 || input.height <= 0 ||
      input.format == AV_PIX_FMT_NONE) {
    throw std::invalid_argument("视频分配器收到无效的输入格式或尺寸");
  }

  if (input.format != AV_PIX_FMT_CUDA) {
    input_format_ = static_cast<AVPixelFormat>(input.format);
    storage_format_ = input_format_;
    input_width_ = input.width;
    input_height_ = input.height;
    prepared_ = true;
    return;
  }

  const auto& input_context = GetCudaFramesContext(input);
  AVBufferRef* pending_output_context =
      av_hwframe_ctx_alloc(input_context.device_ref);
  if (!pending_output_context) {
    throw std::bad_alloc();
  }
  AVBufferRef* pending_device_context = av_buffer_ref(input_context.device_ref);
  if (!pending_device_context) {
    av_buffer_unref(&pending_output_context);
    throw std::bad_alloc();
  }

  auto* output_context =
      reinterpret_cast<AVHWFramesContext*>(pending_output_context->data);
  output_context->format = AV_PIX_FMT_CUDA;
  output_context->sw_format = input_context.sw_format;
  output_context->width = static_cast<int>(output_width_);
  output_context->height = static_cast<int>(output_height_);
  try {
    ffmpeg::ThrowIfError(av_hwframe_ctx_init(pending_output_context),
                         "初始化Processor CUDA输出帧池");
  } catch (...) {
    av_buffer_unref(&pending_device_context);
    av_buffer_unref(&pending_output_context);
    throw;
  }

  input_format_ = AV_PIX_FMT_CUDA;
  storage_format_ = input_context.sw_format;
  input_width_ = input.width;
  input_height_ = input.height;
  input_device_context_ = pending_device_context;
  output_frames_context_ = pending_output_context;
  prepared_ = true;
}

void VideoFrameAllocator::ValidatePreparedInput(const AVFrame& input) const {
  if (input.format != input_format_ || input.width != input_width_ ||
      input.height != input_height_) {
    throw std::invalid_argument("当前链路不支持动态改变视频格式或分辨率");
  }
  if (input_format_ != AV_PIX_FMT_CUDA) {
    return;
  }

  const auto& input_context = GetCudaFramesContext(input);
  if (input_context.sw_format != storage_format_ || !input_device_context_ ||
      input_context.device_ref->data != input_device_context_->data) {
    throw std::invalid_argument("当前链路不支持动态改变CUDA视频格式或设备");
  }
}

}  // namespace mw::streamer::processor::internal

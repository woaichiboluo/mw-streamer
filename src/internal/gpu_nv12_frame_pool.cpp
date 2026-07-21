#include "mw/streamer/internal/gpu_nv12_frame_pool.h"

#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {

void GpuNv12FramePool::Create(const CudaDeviceContext& device_context,
                              int width, int height, int initial_pool_size) {
  if (is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "GPU NV12 frame pool can only be created once");
  }
  if (!device_context.is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "GPU NV12 frame pool requires a valid CUDA device context");
  }
  if (width <= 0 || height <= 0 || initial_pool_size < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "GPU NV12 frame pool dimensions or size are invalid");
  }

  auto device_reference = device_context.buffer().Ref();
  AVBufferRef* raw_frames_context =
      av_hwframe_ctx_alloc(device_reference.get());
  if (raw_frames_context == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "failed to allocate a CUDA frame context");
  }
  auto frames_context = ffmpeg::BufferRef::Adopt(raw_frames_context);

  auto* properties =
      reinterpret_cast<AVHWFramesContext*>(frames_context.get()->data);
  if (properties == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "CUDA frame context is missing its properties");
  }
  properties->format = AV_PIX_FMT_CUDA;
  properties->sw_format = AV_PIX_FMT_NV12;
  properties->width = width;
  properties->height = height;
  properties->initial_pool_size = initial_pool_size;
  CheckFfmpeg(av_hwframe_ctx_init(frames_context.get()),
              "failed to initialize the CUDA NV12 frame pool");

  frames_context_.emplace(std::move(frames_context));
  device_id_ = device_context.device_id();
  width_ = width;
  height_ = height;
}

ffmpeg::Frame GpuNv12FramePool::Acquire() {
  if (!is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "GPU NV12 frame pool has not been created");
  }

  ffmpeg::Frame frame;
  frame.get()->format = AV_PIX_FMT_CUDA;
  frame.get()->width = width_;
  frame.get()->height = height_;
  CheckFfmpeg(av_hwframe_get_buffer(frames_context_->get(), frame.get(), 0),
              "failed to acquire a CUDA NV12 frame");
  if (frame.get()->data[0] == nullptr || frame.get()->data[1] == nullptr ||
      frame.get()->hw_frames_ctx == nullptr) {
    ThrowError(StatusCode::kFfmpegError,
               "CUDA NV12 frame pool returned an incomplete frame");
  }
  return frame;
}

bool GpuNv12FramePool::is_valid() const noexcept {
  return frames_context_.has_value() && frames_context_->get() != nullptr;
}

int GpuNv12FramePool::device_id() const noexcept { return device_id_; }

int GpuNv12FramePool::width() const noexcept { return width_; }

int GpuNv12FramePool::height() const noexcept { return height_; }

const ffmpeg::BufferRef& GpuNv12FramePool::buffer() const noexcept {
  return *frames_context_;
}

}  // namespace mw::streamer::internal

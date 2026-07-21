#include "mw/streamer/internal/cuda_device_context.h"

#include <string>
#include <utility>

#include "fmt/format.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

#include "mw/streamer/ffmpeg/dictionary.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {

void CudaDeviceContext::Create(int device_id) {
  if (is_valid()) {
    ThrowError(StatusCode::kInvalidArgument,
               "CUDA device context can only be created once");
  }
  if (device_id < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "NVIDIA device id must not be negative");
  }

  ffmpeg::Dictionary options;
  options.Set("primary_ctx", "1");
  AVBufferRef* raw_context = nullptr;
  const std::string device_name = fmt::format("{}", device_id);
  const int result =
      av_hwdevice_ctx_create(&raw_context, AV_HWDEVICE_TYPE_CUDA,
                             device_name.c_str(), options.get(), 0);
  if (result < 0) {
    av_buffer_unref(&raw_context);
    ThrowError(StatusCode::kUnavailable,
               fmt::format("failed to open NVIDIA device {}", device_id),
               result);
  }
  if (raw_context == nullptr) {
    ThrowError(StatusCode::kInternal,
               "FFmpeg returned an empty CUDA device context");
  }

  auto candidate = ffmpeg::BufferRef::Adopt(raw_context);
  context_.emplace(std::move(candidate));
  device_id_ = device_id;
}

bool CudaDeviceContext::is_valid() const noexcept {
  return context_.has_value() && context_->get() != nullptr;
}

int CudaDeviceContext::device_id() const noexcept { return device_id_; }

const ffmpeg::BufferRef& CudaDeviceContext::buffer() const noexcept {
  return *context_;
}

}  // namespace mw::streamer::internal

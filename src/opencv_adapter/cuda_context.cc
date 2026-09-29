#include "mw/opencv_adapter/cuda_context.h"

#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

namespace mw::opencv_adapter {

CUcontext GetCudaContext(const MwStreamerExecutionContext& execution) {
  if (execution.type != kMwStreamerExecutionCuda ||
      !execution.ffmpeg_device_context) {
    throw std::invalid_argument("执行上下文不包含FFmpeg CUDA设备上下文");
  }

  const auto* device_context =
      static_cast<const AVHWDeviceContext*>(execution.ffmpeg_device_context);
  if (device_context->type != AV_HWDEVICE_TYPE_CUDA || !device_context->hwctx) {
    throw std::invalid_argument("FFmpeg设备上下文不是有效的CUDA上下文");
  }

  const auto* cuda_device_context =
      static_cast<const AVCUDADeviceContext*>(device_context->hwctx);
  if (!cuda_device_context->cuda_ctx) {
    throw std::invalid_argument("FFmpeg CUDA设备上下文不包含原生context");
  }
  return cuda_device_context->cuda_ctx;
}

}  // namespace mw::opencv_adapter

#include "mw/opencv_adapter/cuda_context.h"

#include <stdexcept>
#include <string>

#include "mw/opencv_adapter/internal/cuda_driver.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

namespace mw::opencv_adapter {
namespace {

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* error_name = nullptr;
  cuGetErrorName(result, &error_name);
  throw std::runtime_error(std::string(operation) + "失败: " +
                           (error_name ? error_name : "CUDA_ERROR_UNKNOWN"));
}

}  // namespace

ScopedCudaContext::ScopedCudaContext(CUcontext context) {
  if (!context) {
    throw std::invalid_argument("要求有效的CUDA context");
  }
  EnsureCudaDriverInitialized();
  CUcontext current = nullptr;
  ThrowIfCudaError(cuCtxGetCurrent(&current), "查询当前CUDA context");
  if (current == context) {
    return;
  }
  ThrowIfCudaError(cuCtxPushCurrent(context), "设置CUDA context");
  pushed_ = true;
}

ScopedCudaContext::ScopedCudaContext(
    const MwStreamerExecutionContext& execution)
    : ScopedCudaContext(GetCudaContext(execution)) {}

ScopedCudaContext::ScopedCudaContext(const MwStreamerVideoBufferView& buffer)
    : ScopedCudaContext(GetCudaContext(buffer)) {}

ScopedCudaContext::~ScopedCudaContext() {
  if (pushed_) {
    CUcontext popped_context = nullptr;
    cuCtxPopCurrent(&popped_context);
  }
}

CUcontext GetCudaContext(const MwStreamerExecutionContext& execution) {
  if (execution.type != kMwStreamerExecutionCuda) {
    throw std::invalid_argument("执行上下文不是CUDA上下文");
  }
  if (execution.native_context) {
    return static_cast<CUcontext>(execution.native_context);
  }
  if (!execution.ffmpeg_device_context) {
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

CUcontext GetCudaContext(const MwStreamerVideoBufferView& buffer) {
  if (buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("视频缓冲区不是CUDA内存");
  }
  return GetCudaContext(buffer.execution);
}

}  // namespace mw::opencv_adapter

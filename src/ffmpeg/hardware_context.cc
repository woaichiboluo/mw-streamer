#include "mw/ffmpeg/hardware_context.h"

#include <cuda.h>
#include <fmt/format.h>

#include <new>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

struct CudaResources {
  CUdevice device = 0;
  CUcontext context = nullptr;
  CUstream stream = nullptr;
  bool primary_context_retained = false;
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

const AVCUDADeviceContext* GetCudaContext(const AVBufferRef* context) noexcept {
  if (!context) {
    return nullptr;
  }
  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(context->data);
  if (!device_context || device_context->type != AV_HWDEVICE_TYPE_CUDA) {
    return nullptr;
  }
  return static_cast<const AVCUDADeviceContext*>(device_context->hwctx);
}

void DestroyCudaResources(CudaResources* resources) noexcept {
  if (!resources || !resources->context) {
    return;
  }

  CUcontext popped_context = nullptr;
  const bool pushed = cuCtxPushCurrent(resources->context) == CUDA_SUCCESS;
  if (pushed && resources->stream) {
    cuStreamSynchronize(resources->stream);
    cuStreamDestroy(resources->stream);
    resources->stream = nullptr;
  }
  if (pushed) {
    cuCtxPopCurrent(&popped_context);
  }
  if (resources->primary_context_retained) {
    cuDevicePrimaryCtxRelease(resources->device);
    resources->primary_context_retained = false;
  }
  resources->context = nullptr;
}

void FreeCudaDeviceContext(AVHWDeviceContext* device_context) {
  auto* resources = static_cast<CudaResources*>(device_context->user_opaque);
  DestroyCudaResources(resources);

  auto* cuda_context = static_cast<AVCUDADeviceContext*>(device_context->hwctx);
  if (cuda_context) {
    cuda_context->cuda_ctx = nullptr;
    cuda_context->stream = nullptr;
  }
  delete resources;
  device_context->user_opaque = nullptr;
}

}  // namespace

HardwareContext::CurrentScope::CurrentScope(const AVBufferRef* context)
    : context_(av_buffer_ref(context)) {
  if (!context_) {
    throw std::bad_alloc();
  }

  const auto* cuda_context = GetCudaContext(context_);
  if (!cuda_context || !cuda_context->cuda_ctx) {
    av_buffer_unref(&context_);
    throw std::logic_error("HardwareContext不是有效的CUDA上下文");
  }

  CUcontext current_context = nullptr;
  const CUresult current_result = cuCtxGetCurrent(&current_context);
  if (current_result != CUDA_SUCCESS) {
    av_buffer_unref(&context_);
    ThrowIfCudaError(current_result, "获取当前CUDA上下文");
  }
  if (current_context == cuda_context->cuda_ctx) {
    return;
  }

  const CUresult result = cuCtxPushCurrent(cuda_context->cuda_ctx);
  if (result != CUDA_SUCCESS) {
    av_buffer_unref(&context_);
    ThrowIfCudaError(result, "设置当前CUDA上下文");
  }
  pushed_ = true;
}

HardwareContext::CurrentScope::~CurrentScope() {
  if (!context_) {
    return;
  }
  if (pushed_) {
    CUcontext popped_context = nullptr;
    cuCtxPopCurrent(&popped_context);
  }
  av_buffer_unref(&context_);
}

HardwareContext HardwareContext::CreateCuda(int device_index) {
  if (device_index < 0) {
    throw std::invalid_argument("CUDA设备索引不能为负数");
  }

  AVBufferRef* device_ref = nullptr;
  CudaResources pending_resources;
  try {
    ThrowIfCudaError(cuInit(0), "初始化CUDA Driver");

    ThrowIfCudaError(cuDeviceGet(&pending_resources.device, device_index),
                     "获取CUDA设备");
    ThrowIfCudaError(cuDevicePrimaryCtxRetain(&pending_resources.context,
                                              pending_resources.device),
                     "保留CUDA Primary Context");
    pending_resources.primary_context_retained = true;

    ThrowIfCudaError(cuCtxPushCurrent(pending_resources.context),
                     "设置CUDA Primary Context");
    const CUresult stream_result =
        cuStreamCreate(&pending_resources.stream, CU_STREAM_NON_BLOCKING);
    int create_result = 0;
    if (stream_result == CUDA_SUCCESS) {
      const auto device_name = fmt::format("{}", device_index);
      create_result = av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_CUDA,
                                             device_name.c_str(), nullptr,
                                             AV_CUDA_USE_CURRENT_CONTEXT);
    }
    CUcontext popped_context = nullptr;
    const CUresult pop_result = cuCtxPopCurrent(&popped_context);

    ThrowIfCudaError(stream_result, "创建CUDA Stream");
    ThrowIfError(create_result, "创建FFmpeg CUDA硬件上下文");
    ThrowIfCudaError(pop_result, "恢复原CUDA上下文");

    auto* device_context =
        reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto* cuda_context =
        static_cast<AVCUDADeviceContext*>(device_context->hwctx);
    if (device_context->free || device_context->user_opaque) {
      throw std::logic_error("FFmpeg CUDA硬件上下文已设置资源释放器");
    }
    cuda_context->stream = pending_resources.stream;

    auto* owned_resources = new CudaResources(pending_resources);
    device_context->free = FreeCudaDeviceContext;
    device_context->user_opaque = owned_resources;
    pending_resources = {};
  } catch (...) {
    DestroyCudaResources(&pending_resources);
    av_buffer_unref(&device_ref);
    throw;
  }

  return HardwareContext(device_ref, device_index);
}

HardwareContext::HardwareContext(AVBufferRef* context, int device_index)
    : context_(context), device_index_(device_index) {}

HardwareContext::~HardwareContext() { av_buffer_unref(&context_); }

HardwareContext::HardwareContext(const HardwareContext& other) {
  if (!other.context_) {
    throw std::logic_error("不能引用已移动的HardwareContext");
  }
  context_ = av_buffer_ref(other.context_);
  if (!context_) {
    throw std::bad_alloc();
  }
  device_index_ = other.device_index_;
}

HardwareContext& HardwareContext::operator=(const HardwareContext& other) {
  if (this != &other) {
    HardwareContext copy(other);
    std::swap(context_, copy.context_);
    std::swap(device_index_, copy.device_index_);
  }
  return *this;
}

HardwareContext::HardwareContext(HardwareContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      device_index_(std::exchange(other.device_index_, -1)) {}

HardwareContext& HardwareContext::operator=(HardwareContext&& other) noexcept {
  if (this != &other) {
    av_buffer_unref(&context_);
    context_ = std::exchange(other.context_, nullptr);
    device_index_ = std::exchange(other.device_index_, -1);
  }
  return *this;
}

AVHWDeviceType HardwareContext::type() const noexcept {
  if (!context_) {
    return AV_HWDEVICE_TYPE_NONE;
  }
  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(context_->data);
  return device_context ? device_context->type : AV_HWDEVICE_TYPE_NONE;
}

int HardwareContext::device_index() const noexcept { return device_index_; }

const AVBufferRef* HardwareContext::get() const noexcept { return context_; }

std::uintptr_t HardwareContext::native_stream() const noexcept {
  const auto* cuda_context = GetCudaContext(context_);
  return cuda_context ? reinterpret_cast<std::uintptr_t>(cuda_context->stream)
                      : 0;
}

HardwareContext::CurrentScope HardwareContext::MakeCurrent() const {
  if (!context_) {
    throw std::logic_error("不能使用已移动的HardwareContext");
  }
  return CurrentScope(context_);
}

}  // namespace mw::streamer::ffmpeg

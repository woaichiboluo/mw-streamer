#include <cuda.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"

namespace {

using mw::streamer::ffmpeg::HardwareContext;

const AVCUDADeviceContext* GetCudaContext(const HardwareContext& context) {
  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(context.get()->data);
  return static_cast<const AVCUDADeviceContext*>(device_context->hwctx);
}

TEST_CASE("HardwareContext共享CUDA Primary Context并创建独立流") {
  const auto context = HardwareContext::CreateCuda(0);
  const auto other_context = HardwareContext::CreateCuda(0);
  const auto* cuda_context = GetCudaContext(context);
  const auto* other_cuda_context = GetCudaContext(other_context);

  CHECK(context.type() == AV_HWDEVICE_TYPE_CUDA);
  CHECK(context.device_index() == 0);
  REQUIRE(cuda_context != nullptr);
  REQUIRE(other_cuda_context != nullptr);
  CHECK(cuda_context->cuda_ctx != nullptr);
  CHECK(cuda_context->stream != nullptr);
  CHECK(cuda_context->cuda_ctx == other_cuda_context->cuda_ctx);
  CHECK(cuda_context->stream != other_cuda_context->stream);
  CHECK(context.native_handle() ==
        reinterpret_cast<std::uintptr_t>(cuda_context->stream));
}

TEST_CASE("HardwareContext拷贝共享底层CUDA资源") {
  const auto original = HardwareContext::CreateCuda(0);
  const auto copy = original;

  CHECK(copy.get() != original.get());
  CHECK(copy.get()->data == original.get()->data);
  CHECK(copy.native_handle() == original.native_handle());
  CHECK(copy.device_index() == original.device_index());
}

TEST_CASE("FFmpeg引用可独立维持HardwareContext资源生命周期") {
  AVBufferRef* retained_context = nullptr;
  std::uintptr_t stream = 0;
  {
    const auto context = HardwareContext::CreateCuda(0);
    retained_context = av_buffer_ref(context.get());
    REQUIRE(retained_context != nullptr);
    stream = context.native_handle();
  }

  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(retained_context->data);
  const auto* cuda_context =
      static_cast<const AVCUDADeviceContext*>(device_context->hwctx);
  CHECK(reinterpret_cast<std::uintptr_t>(cuda_context->stream) == stream);
  CHECK(cuda_context->cuda_ctx != nullptr);

  av_buffer_unref(&retained_context);
}

TEST_CASE("HardwareContext在作用域内切换并恢复当前上下文") {
  const auto context = HardwareContext::CreateCuda(0);
  const auto* cuda_context = GetCudaContext(context);

  CUcontext previous = nullptr;
  REQUIRE(cuCtxGetCurrent(&previous) == CUDA_SUCCESS);
  {
    const auto current_scope = context.MakeCurrent();
    CUcontext current = nullptr;
    REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current == cuda_context->cuda_ctx);
  }

  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  CHECK(restored == previous);
}

TEST_CASE("HardwareContext同步自身执行流并恢复当前上下文") {
  const auto context = HardwareContext::CreateCuda(0);
  const auto* cuda_context = GetCudaContext(context);
  REQUIRE(cuda_context != nullptr);

  CUevent completion = nullptr;
  {
    const auto current_scope = context.MakeCurrent();
    REQUIRE(cuEventCreate(&completion, CU_EVENT_DISABLE_TIMING) ==
            CUDA_SUCCESS);
    REQUIRE(cuEventRecord(completion, cuda_context->stream) == CUDA_SUCCESS);
  }

  CUcontext previous = nullptr;
  REQUIRE(cuCtxGetCurrent(&previous) == CUDA_SUCCESS);
  context.Synchronize();
  CHECK(cuEventQuery(completion) == CUDA_SUCCESS);

  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  CHECK(restored == previous);
  {
    const auto current_scope = context.MakeCurrent();
    CHECK(cuEventDestroy(completion) == CUDA_SUCCESS);
  }
}

TEST_CASE("HardwareContext可供FFmpeg分配CUDA帧") {
  const auto context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(context.get()));
  REQUIRE(frames_ref != nullptr);

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = 64;
  frames_context->height = 64;
  frames_context->initial_pool_size = 1;

  const int init_result = av_hwframe_ctx_init(frames_ref);
  if (init_result < 0) {
    av_buffer_unref(&frames_ref);
  }
  REQUIRE(init_result >= 0);

  mw::streamer::ffmpeg::Frame frame;
  const int allocate_result = av_hwframe_get_buffer(frames_ref, frame.get(), 0);
  av_buffer_unref(&frames_ref);

  REQUIRE(allocate_result >= 0);
  CHECK(frame->format == AV_PIX_FMT_CUDA);
  CHECK(frame->hw_frames_ctx != nullptr);
  const auto* mapped_context = HardwareContext::GetFramesContext(*frame.get());
  REQUIRE(mapped_context != nullptr);
  CHECK(mapped_context->sw_format == AV_PIX_FMT_NV12);
  CHECK(context.IsCompatible(*frame.get()));

  const auto other_context = HardwareContext::CreateCuda(0);
  CHECK_FALSE(other_context.IsCompatible(*frame.get()));

  mw::streamer::ffmpeg::Frame software_frame;
  software_frame->format = AV_PIX_FMT_NV12;
  CHECK(HardwareContext::GetFramesContext(*software_frame.get()) == nullptr);
}

TEST_CASE("HardwareContext拒绝无效CUDA设备索引") {
  CHECK_THROWS_AS(HardwareContext::CreateCuda(-1), std::invalid_argument);
  CHECK_THROWS(HardwareContext::CreateCuda(99999));
}

}  // namespace

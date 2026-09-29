#include "mw/opencv_adapter/cuda_context.h"

#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/streamer/ffmpeg/hardware_context.h"

namespace {

using mw::opencv_adapter::GetCudaContext;
using mw::streamer::HardwareContext;

TEST_CASE("OpenCV Adapter从FFmpeg执行上下文取得可用CUDA context") {
  const auto hardware_context = HardwareContext::CreateCuda(0);
  const MwStreamerExecutionContext execution{kMwStreamerExecutionCuda,
                                             hardware_context.get()->data};

  const auto* device_context =
      static_cast<const AVHWDeviceContext*>(execution.ffmpeg_device_context);
  const auto* cuda_device_context =
      static_cast<const AVCUDADeviceContext*>(device_context->hwctx);
  const CUcontext context = GetCudaContext(execution);
  REQUIRE(context == cuda_device_context->cuda_ctx);

  REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
  CUcontext current = nullptr;
  const CUresult get_result = cuCtxGetCurrent(&current);
  CUcontext popped = nullptr;
  const CUresult pop_result = cuCtxPopCurrent(&popped);
  CHECK(get_result == CUDA_SUCCESS);
  CHECK(current == context);
  REQUIRE(pop_result == CUDA_SUCCESS);
  CHECK(popped == context);
}

TEST_CASE("OpenCV Adapter拒绝不含有效CUDA context的执行上下文") {
  CHECK_THROWS_AS(GetCudaContext({kMwStreamerExecutionCpu, nullptr}),
                  std::invalid_argument);
  CHECK_THROWS_AS(GetCudaContext({kMwStreamerExecutionCuda, nullptr}),
                  std::invalid_argument);

  AVHWDeviceContext device_context{};
  device_context.type = AV_HWDEVICE_TYPE_NONE;
  CHECK_THROWS_AS(GetCudaContext({kMwStreamerExecutionCuda, &device_context}),
                  std::invalid_argument);

  device_context.type = AV_HWDEVICE_TYPE_CUDA;
  CHECK_THROWS_AS(GetCudaContext({kMwStreamerExecutionCuda, &device_context}),
                  std::invalid_argument);

  AVCUDADeviceContext cuda_device_context{};
  device_context.hwctx = &cuda_device_context;
  CHECK_THROWS_AS(GetCudaContext({kMwStreamerExecutionCuda, &device_context}),
                  std::invalid_argument);
}

}  // namespace

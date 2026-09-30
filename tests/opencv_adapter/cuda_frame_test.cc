#include <cuda.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/streamer/ffmpeg/hardware_context.h"

namespace {

using mw::opencv_adapter::CudaFrame;
using mw::opencv_adapter::HostFrame;
using mw::streamer::HardwareContext;

static_assert(!std::is_copy_constructible_v<CudaFrame>);
static_assert(!std::is_copy_assignable_v<CudaFrame>);
static_assert(std::is_nothrow_move_constructible_v<CudaFrame>);
static_assert(std::is_nothrow_move_assignable_v<CudaFrame>);

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result != CUDA_SUCCESS) {
    throw std::runtime_error(operation);
  }
}

class ScopedCudaContext final {
 public:
  explicit ScopedCudaContext(CUcontext context) {
    ThrowIfCudaError(cuCtxPushCurrent(context), "设置测试CUDA context失败");
  }

  ~ScopedCudaContext() {
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
  }

  ScopedCudaContext(const ScopedCudaContext&) = delete;
  ScopedCudaContext& operator=(const ScopedCudaContext&) = delete;
};

class CudaEnvironment final {
 public:
  CudaEnvironment() : hardware_context_(HardwareContext::CreateCuda(0)) {
    const auto* device = reinterpret_cast<const AVHWDeviceContext*>(
        hardware_context_.get()->data);
    const auto* cuda_device =
        static_cast<const AVCUDADeviceContext*>(device->hwctx);
    context_ = cuda_device->cuda_ctx;
    ScopedCudaContext scoped_context(context_);
    ThrowIfCudaError(cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING),
                     "创建测试CUDA stream失败");
  }

  ~CudaEnvironment() {
    if (stream_) {
      ScopedCudaContext scoped_context(context_);
      cuStreamDestroy(stream_);
    }
  }

  CudaEnvironment(const CudaEnvironment&) = delete;
  CudaEnvironment& operator=(const CudaEnvironment&) = delete;

  CUcontext context() const noexcept { return context_; }
  CUstream stream() const noexcept { return stream_; }

  void Synchronize() const {
    ScopedCudaContext scoped_context(context_);
    ThrowIfCudaError(cuStreamSynchronize(stream_), "同步测试CUDA stream失败");
  }

 private:
  HardwareContext hardware_context_;
  CUcontext context_ = nullptr;
  CUstream stream_ = nullptr;
};

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

MwStreamerMediaTimestamp MakeTimestamp() { return {1234, 40, {1, 1000}}; }

class HostNv12Frame final {
 public:
  HostNv12Frame(std::uint32_t width, std::uint32_t height, std::uint8_t y_value,
                std::uint8_t uv_value)
      : y_(static_cast<std::size_t>(width) * height, y_value),
        uv_(static_cast<std::size_t>(width) * height / 2, uv_value),
        planes_({{{reinterpret_cast<std::uintptr_t>(y_.data()),
                   static_cast<std::int32_t>(width), width, height},
                  {reinterpret_cast<std::uintptr_t>(uv_.data()),
                   static_cast<std::int32_t>(width), width, height / 2}}}),
        view_({{kMwStreamerMemoryHost,
                {kMwStreamerExecutionCpu, nullptr, nullptr},
                kMwStreamerVideoStorageLinear,
                kMwStreamerVideoPixelFormatNv12,
                width,
                height,
                {{planes_.data(), static_cast<std::uint32_t>(planes_.size())}}},
               MakeColorInfo(),
               MakeTimestamp()}) {}

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }

 private:
  std::vector<std::uint8_t> y_;
  std::vector<std::uint8_t> uv_;
  std::array<MwStreamerVideoPlaneView, 2> planes_{};
  MwStreamerVideoFrameView view_{};
};

HostFrame MakePinnedCopy(const MwStreamerVideoFrameView& source,
                         CUcontext context) {
  auto result = HostFrame::AllocatePinned(source, context);
  HostFrame::Copy(source, result.view().buffer);
  return result;
}

void CheckNv12Values(const MwStreamerVideoFrameView& frame,
                     std::uint8_t y_value, std::uint8_t uv_value) {
  const auto& linear = frame.buffer.storage.linear;
  REQUIRE(linear.plane_count == 2);
  const auto* y =
      reinterpret_cast<const std::uint8_t*>(linear.planes[0].address);
  const auto* uv =
      reinterpret_cast<const std::uint8_t*>(linear.planes[1].address);
  CHECK(y[0] == y_value);
  CHECK(y[linear.planes[0].row_bytes * linear.planes[0].row_count - 1] ==
        y_value);
  CHECK(uv[0] == uv_value);
  CHECK(uv[linear.planes[1].row_bytes * linear.planes[1].row_count - 1] ==
        uv_value);
}

TEST_CASE("CudaFrame在显式context和stream中异步复制") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  HostNv12Frame source(kWidth, kHeight, 0x31, 0x72);
  HostNv12Frame empty(kWidth, kHeight, 0, 0);

  auto pinned_source = MakePinnedCopy(source.view(), cuda.context());
  auto pinned_destination = MakePinnedCopy(empty.view(), cuda.context());
  auto cuda_source = CudaFrame::Allocate(source.view(), cuda.context());
  auto cuda_destination = CudaFrame::Allocate(source.view(), cuda.context());

  REQUIRE(cuCtxSetCurrent(nullptr) == CUDA_SUCCESS);
  cuda_source.CopyFrom(pinned_source.view(), cuda.stream());
  cuda_destination.CopyFrom(cuda_source.view(), cuda.stream());
  cuda_destination.CopyTo(pinned_destination.view().buffer, cuda.stream());

  CUcontext current = nullptr;
  REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
  CHECK(current == nullptr);
  cuda.Synchronize();

  CHECK(cuda_source.context() == cuda.context());
  CHECK(cuda_source.view().buffer.memory_type == kMwStreamerMemoryCuda);
  CHECK(cuda_source.view().buffer.execution.type == kMwStreamerExecutionCuda);
  CHECK(cuda_source.view().buffer.execution.native_context == cuda.context());
  CHECK(cuda_source.view().color.space == kMwStreamerColorSpaceBt709);
  CHECK(cuda_source.view().timestamp.pts == 1234);
  CheckNv12Values(pinned_destination.view(), 0x31, 0x72);

  auto moved = std::move(cuda_destination);
  CHECK(moved.context() == cuda.context());
}

TEST_CASE("CudaFrame支持pageable Host内存并拒绝负stride") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  HostNv12Frame pageable(kWidth, kHeight, 0x31, 0x72);
  HostNv12Frame output(kWidth, kHeight, 0, 0);
  auto destination = CudaFrame::Allocate(pageable.view(), cuda.context());

  destination.CopyFrom(pageable.view(), cuda.stream());
  destination.CopyTo(output.view().buffer, cuda.stream());
  cuda.Synchronize();
  CheckNv12Values(output.view(), 0x31, 0x72);

  auto pinned = MakePinnedCopy(pageable.view(), cuda.context());
  auto invalid_source = pinned.view();
  std::array<MwStreamerVideoPlaneView, 2> invalid_planes = {
      invalid_source.buffer.storage.linear.planes[0],
      invalid_source.buffer.storage.linear.planes[1]};
  invalid_planes[0].stride_bytes = -invalid_planes[0].stride_bytes;
  invalid_source.buffer.storage.linear = {
      invalid_planes.data(), static_cast<std::uint32_t>(invalid_planes.size())};
  CHECK_THROWS_AS(destination.CopyFrom(invalid_source, cuda.stream()),
                  std::invalid_argument);
}

TEST_CASE("CudaFrame省略stream时提交到指定context的default stream") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  HostNv12Frame source(kWidth, kHeight, 0x31, 0x72);
  HostNv12Frame empty(kWidth, kHeight, 0, 0);
  auto pinned_source = MakePinnedCopy(source.view(), cuda.context());
  auto pinned_destination = MakePinnedCopy(empty.view(), cuda.context());
  auto cuda_frame = CudaFrame::Allocate(source.view(), cuda.context());

  std::atomic<bool> preceding_work_finished = false;
  {
    ScopedCudaContext scoped_context(cuda.context());
    REQUIRE(cuLaunchHostFunc(
                nullptr,
                [](void* state) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(100));
                  static_cast<std::atomic<bool>*>(state)->store(true);
                },
                &preceding_work_finished) == CUDA_SUCCESS);
  }

  REQUIRE(cuCtxSetCurrent(nullptr) == CUDA_SUCCESS);
  cuda_frame.CopyFrom(pinned_source.view());
  cuda_frame.CopyTo(pinned_destination.view().buffer);
  CHECK_FALSE(preceding_work_finished.load());

  CUcontext current = nullptr;
  REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
  CHECK(current == nullptr);
  {
    ScopedCudaContext scoped_context(cuda.context());
    REQUIRE(cuStreamSynchronize(nullptr) == CUDA_SUCCESS);
  }
  CHECK(preceding_work_finished.load());
  CheckNv12Values(pinned_destination.view(), 0x31, 0x72);
}

TEST_CASE("CudaFrame拒绝跨context和不匹配stream") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment first;
  CudaEnvironment second;
  HostNv12Frame prototype(kWidth, kHeight, 0, 0);
  auto first_frame = CudaFrame::Allocate(prototype.view(), first.context());
  auto second_frame = CudaFrame::Allocate(prototype.view(), second.context());

  CHECK(first.context() != second.context());
  CHECK_THROWS_AS(CudaFrame::Copy(first_frame.view(),
                                  second_frame.view().buffer, first.stream()),
                  std::invalid_argument);
  CHECK_THROWS_AS(CudaFrame::Copy(first_frame.view(), first_frame.view().buffer,
                                  second.stream()),
                  std::invalid_argument);
}

TEST_CASE("CudaFrame校验原型和输出布局") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  HostNv12Frame prototype(kWidth, kHeight, 0, 0);

  auto invalid_prototype = prototype.view();
  invalid_prototype.buffer.storage_type = kMwStreamerVideoStorageNativeSurface;
  CHECK_THROWS_AS(CudaFrame::Allocate(invalid_prototype, cuda.context()),
                  std::invalid_argument);
  invalid_prototype = prototype.view();
  invalid_prototype.buffer.storage.linear = {nullptr, 0};
  CHECK_THROWS_AS(CudaFrame::Allocate(invalid_prototype, cuda.context()),
                  std::invalid_argument);
  CHECK_THROWS_AS(CudaFrame::Allocate(prototype.view(), nullptr),
                  std::invalid_argument);

  auto source = CudaFrame::Allocate(prototype.view(), cuda.context());
  auto destination = CudaFrame::Allocate(prototype.view(), cuda.context());
  auto invalid_destination = destination.view().buffer;
  invalid_destination.width -= 1;
  CHECK_THROWS_AS(
      CudaFrame::Copy(source.view(), invalid_destination, cuda.stream()),
      std::invalid_argument);
  invalid_destination = destination.view().buffer;
  invalid_destination.memory_type = static_cast<MwStreamerMemoryType>(999);
  CHECK_THROWS_AS(
      CudaFrame::Copy(source.view(), invalid_destination, cuda.stream()),
      std::invalid_argument);
}

}  // namespace

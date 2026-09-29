#include <cuda.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/opencv_adapter/host_frame.h"
#include "mw/streamer/ffmpeg/hardware_context.h"

namespace {

using mw::opencv_adapter::HostFrame;
using mw::streamer::HardwareContext;

static_assert(!std::is_copy_constructible_v<HostFrame>);
static_assert(!std::is_copy_assignable_v<HostFrame>);
static_assert(std::is_nothrow_move_constructible_v<HostFrame>);
static_assert(std::is_nothrow_move_assignable_v<HostFrame>);

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
  }

  CUcontext context() const noexcept { return context_; }

 private:
  HardwareContext hardware_context_;
  CUcontext context_ = nullptr;
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

TEST_CASE("HostFrame复制紧密Host帧并按需显式分配page-locked内存") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  ScopedCudaContext scoped_context(cuda.context());
  HostNv12Frame source(kWidth, kHeight, 0x31, 0x72);
  HostNv12Frame empty(kWidth, kHeight, 0, 0);

  auto frame = HostFrame::CopyFrom(source.view());
  CHECK(frame.view().buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(frame.view().buffer.pixel_format == kMwStreamerVideoPixelFormatNv12);
  CHECK(frame.view().color.space == kMwStreamerColorSpaceBt709);
  CHECK(frame.view().timestamp.pts == 1234);
  CheckNv12Values(frame.view(), 0x31, 0x72);

  auto pinned = HostFrame::AllocatePinned(source.view(), cuda.context());
  HostFrame::Copy(source.view(), pinned.view().buffer);
  CheckNv12Values(pinned.view(), 0x31, 0x72);
  for (std::uint32_t index = 0;
       index < pinned.view().buffer.storage.linear.plane_count; ++index) {
    CUmemorytype memory_type = CU_MEMORYTYPE_DEVICE;
    REQUIRE(
        cuPointerGetAttribute(
            &memory_type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
            static_cast<CUdeviceptr>(
                pinned.view().buffer.storage.linear.planes[index].address)) ==
        CUDA_SUCCESS);
    CHECK(memory_type == CU_MEMORYTYPE_HOST);
  }

  auto destination = empty.view().buffer;
  frame.CopyTo(destination);
  MwStreamerVideoFrameView copied = empty.view();
  copied.buffer = destination;
  CheckNv12Values(copied, 0x31, 0x72);

  auto moved = std::move(frame);
  CheckNv12Values(moved.view(), 0x31, 0x72);
}

TEST_CASE("HostFrame拒绝padding和负stride") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  ScopedCudaContext scoped_context(cuda.context());
  HostNv12Frame source(kWidth, kHeight, 0x31, 0x72);

  auto invalid_source = source.view();
  std::array<MwStreamerVideoPlaneView, 2> invalid_source_planes = {
      invalid_source.buffer.storage.linear.planes[0],
      invalid_source.buffer.storage.linear.planes[1]};
  invalid_source_planes[0].stride_bytes += 1;
  invalid_source.buffer.storage.linear = {
      invalid_source_planes.data(),
      static_cast<std::uint32_t>(invalid_source_planes.size())};
  CHECK_THROWS_AS(HostFrame::CopyFrom(invalid_source), std::invalid_argument);

  invalid_source_planes[0] = source.view().buffer.storage.linear.planes[0];
  invalid_source_planes[0].stride_bytes =
      -invalid_source_planes[0].stride_bytes;
  CHECK_THROWS_AS(HostFrame::CopyFrom(invalid_source), std::invalid_argument);

  auto frame = HostFrame::CopyFrom(source.view());
  HostNv12Frame output(kWidth, kHeight, 0, 0);
  auto invalid_destination = output.view().buffer;
  std::array<MwStreamerVideoPlaneView, 2> invalid_destination_planes = {
      invalid_destination.storage.linear.planes[0],
      invalid_destination.storage.linear.planes[1]};
  invalid_destination_planes[0].stride_bytes += 1;
  invalid_destination.storage.linear = {
      invalid_destination_planes.data(),
      static_cast<std::uint32_t>(invalid_destination_planes.size())};
  CHECK_THROWS_AS(frame.CopyTo(invalid_destination), std::invalid_argument);

  invalid_destination_planes[0] = output.view().buffer.storage.linear.planes[0];
  invalid_destination_planes[0].stride_bytes =
      -invalid_destination_planes[0].stride_bytes;
  CHECK_THROWS_AS(frame.CopyTo(invalid_destination), std::invalid_argument);
}

TEST_CASE("HostFrame拒绝CUDA和无效布局") {
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  CudaEnvironment cuda;
  ScopedCudaContext scoped_context(cuda.context());
  HostNv12Frame source(kWidth, kHeight, 0x31, 0x72);

  auto invalid_source = source.view();
  invalid_source.buffer.memory_type = kMwStreamerMemoryCuda;
  CHECK_THROWS_AS(HostFrame::CopyFrom(invalid_source), std::invalid_argument);
  invalid_source = source.view();
  invalid_source.buffer.storage_type = kMwStreamerVideoStorageNativeSurface;
  CHECK_THROWS_AS(HostFrame::CopyFrom(invalid_source), std::invalid_argument);
  invalid_source = source.view();
  invalid_source.buffer.storage.linear = {nullptr, 0};
  CHECK_THROWS_AS(HostFrame::CopyFrom(invalid_source), std::invalid_argument);

  auto frame = HostFrame::CopyFrom(source.view());
  HostNv12Frame output(kWidth, kHeight, 0, 0);
  auto invalid_destination = output.view().buffer;
  invalid_destination.pixel_format = kMwStreamerVideoPixelFormatP010;
  CHECK_THROWS_AS(frame.CopyTo(invalid_destination), std::invalid_argument);
  invalid_destination = output.view().buffer;
  invalid_destination.width -= 1;
  CHECK_THROWS_AS(frame.CopyTo(invalid_destination), std::invalid_argument);
  invalid_destination = output.view().buffer;
  invalid_destination.storage.linear.plane_count = 1;
  CHECK_THROWS_AS(frame.CopyTo(invalid_destination), std::invalid_argument);
}

}  // namespace

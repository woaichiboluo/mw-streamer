#include <cuda.h>
#include <cuda_runtime_api.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/processor/internal/frame_adapter.h"

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::ffmpeg::ThrowIfError;
using mw::streamer::opencv_adapter::CudaFrame;
using mw::streamer::processor::internal::VideoBufferAdapter;
using mw::streamer::processor::internal::VideoFrameAdapter;

static_assert(!std::is_copy_constructible_v<CudaFrame>);
static_assert(!std::is_copy_assignable_v<CudaFrame>);
static_assert(std::is_nothrow_move_constructible_v<CudaFrame>);
static_assert(std::is_nothrow_move_assignable_v<CudaFrame>);

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

MwStreamerMediaTimestamp MakeTimestamp() { return {1234, 40, {1, 1000}}; }

TEST_CASE("CudaFrame同步上传带padding和负stride的Host帧") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  REQUIRE(cuCtxSetCurrent(nullptr) == CUDA_SUCCESS);

  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  constexpr std::uint32_t kRowBytes = kWidth * 2;
  constexpr std::int32_t kStride = 10;
  std::vector<std::uint8_t> y(kStride * kHeight, 0xee);
  std::vector<std::uint8_t> uv(kStride * (kHeight / 2), 0xdd);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    for (std::uint32_t column = 0; column < kRowBytes; ++column) {
      y[(kHeight - 1 - row) * kStride + column] =
          static_cast<std::uint8_t>(row * 20 + column);
    }
  }
  for (std::uint32_t row = 0; row < kHeight / 2; ++row) {
    for (std::uint32_t column = 0; column < kRowBytes; ++column) {
      uv[row * kStride + column] =
          static_cast<std::uint8_t>(100 + row * 20 + column);
    }
  }

  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data() + (kHeight - 1) * kStride),
       -kStride, kRowBytes, kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kStride, kRowBytes,
       kHeight / 2},
  }};
  const MwStreamerVideoFrameView source = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatP010,
       kWidth,
       kHeight,
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };

  auto cuda = CudaFrame::CopyFrom(source);
  std::memset(y.data(), 0, y.size());
  std::memset(uv.data(), 0, uv.size());

  const auto& cuda_view = cuda.view();
  CHECK(cuda_view.buffer.memory_type == kMwStreamerMemoryCuda);
  CHECK(cuda_view.buffer.pixel_format == kMwStreamerVideoPixelFormatP010);
  CHECK(cuda_view.color.space == kMwStreamerColorSpaceBt709);
  CHECK(cuda_view.timestamp.pts == 1234);
  REQUIRE(cuda_view.buffer.storage.linear.plane_count == 2);
  for (std::uint32_t index = 0; index < 2; ++index) {
    const auto& plane = cuda_view.buffer.storage.linear.planes[index];
    CHECK(plane.stride_bytes >= static_cast<std::int32_t>(plane.row_bytes));
  }

  std::vector<std::uint8_t> output_y(kStride * kHeight, 0xff);
  std::vector<std::uint8_t> output_uv(kStride * (kHeight / 2), 0xff);
  const std::array<MwStreamerVideoPlaneView, 2> output_planes = {{
      {reinterpret_cast<std::uintptr_t>(output_y.data() +
                                        (kHeight - 1) * kStride),
       -kStride, kRowBytes, kHeight},
      {reinterpret_cast<std::uintptr_t>(output_uv.data()), kStride, kRowBytes,
       kHeight / 2},
  }};
  auto host_output = source.buffer;
  host_output.storage.linear = {
      output_planes.data(), static_cast<std::uint32_t>(output_planes.size())};
  cuda.CopyTo(host_output);
  CHECK(output_y[(kHeight - 1) * kStride] == 0);
  CHECK(output_y[7] == 67);
  CHECK(output_uv[0] == 100);
  CHECK(output_uv[kStride + 7] == 127);

  auto cuda_copy = CudaFrame::CopyFrom(source);
  cuda.CopyTo(cuda_copy.view().buffer);
  const auto host = cuda_copy.ToHost();
  const auto& host_view = host.view();
  const auto* copied_y = reinterpret_cast<const std::uint8_t*>(
      host_view.buffer.storage.linear.planes[0].address);
  const auto* copied_uv = reinterpret_cast<const std::uint8_t*>(
      host_view.buffer.storage.linear.planes[1].address);
  CHECK(copied_y[0] == 0);
  CHECK(copied_y[3 * kRowBytes + 7] == 67);
  CHECK(copied_uv[0] == 100);
  CHECK(copied_uv[kRowBytes + 7] == 127);

  auto moved = std::move(cuda_copy);
  CHECK(moved.view().buffer.memory_type == kMwStreamerMemoryCuda);
}

TEST_CASE("CudaFrame跨context深拷贝CUDA帧并脱离源帧生命周期") {
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;
  auto cached = [&]() {
    const auto hardware_context = HardwareContext::CreateCuda(0);
    AVBufferRef* frames_ref =
        av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
    REQUIRE(frames_ref != nullptr);

    auto* frames_context =
        reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    frames_context->format = AV_PIX_FMT_CUDA;
    frames_context->sw_format = AV_PIX_FMT_NV12;
    frames_context->width = kWidth;
    frames_context->height = kHeight;
    frames_context->initial_pool_size = 1;
    ThrowIfError(av_hwframe_ctx_init(frames_ref), "初始化测试CUDA帧池");

    Frame source;
    source->format = AV_PIX_FMT_NV12;
    source->width = kWidth;
    source->height = kHeight;
    ThrowIfError(av_frame_get_buffer(source.get(), 32), "分配测试Host源帧");
    std::memset(source->data[0], 0x36,
                static_cast<std::size_t>(source->linesize[0]) * kHeight);
    std::memset(source->data[1], 0x63,
                static_cast<std::size_t>(source->linesize[1]) * (kHeight / 2));

    Frame cuda_source;
    ThrowIfError(av_hwframe_get_buffer(frames_ref, cuda_source.get(), 0),
                 "分配测试CUDA帧");
    ThrowIfError(av_hwframe_transfer_data(cuda_source.get(), source.get(), 0),
                 "上传测试CUDA帧");
    cuda_source->time_base = {1, 25};
    cuda_source->pts = 9;
    cuda_source->duration = 1;

    const VideoFrameAdapter adapter(cuda_source);
    const auto& source_view = adapter.view();
    CUcontext source_context = nullptr;
    REQUIRE(cuPointerGetAttribute(
                &source_context, CU_POINTER_ATTRIBUTE_CONTEXT,
                static_cast<CUdeviceptr>(
                    source_view.buffer.storage.linear.planes[0].address)) ==
            CUDA_SUCCESS);

    REQUIRE(cudaSetDevice(0) == cudaSuccess);
    CUcontext destination_context = nullptr;
    REQUIRE(cuCtxGetCurrent(&destination_context) == CUDA_SUCCESS);
    REQUIRE(destination_context != nullptr);
    CHECK(destination_context != source_context);
    auto result = CudaFrame::CopyFrom(source_view);

    std::memset(source->data[0], 0,
                static_cast<std::size_t>(source->linesize[0]) * kHeight);
    std::memset(source->data[1], 0,
                static_cast<std::size_t>(source->linesize[1]) * (kHeight / 2));
    ThrowIfError(av_hwframe_transfer_data(cuda_source.get(), source.get(), 0),
                 "清空测试CUDA输出帧");
    const VideoBufferAdapter output_adapter(cuda_source);
    result.CopyTo(output_adapter.view());

    Frame copied_host;
    copied_host->format = AV_PIX_FMT_NV12;
    copied_host->width = kWidth;
    copied_host->height = kHeight;
    ThrowIfError(av_frame_get_buffer(copied_host.get(), 32),
                 "分配测试Host目标帧");
    ThrowIfError(
        av_hwframe_transfer_data(copied_host.get(), cuda_source.get(), 0),
        "下载跨context复制结果");
    CHECK(copied_host->data[0][0] == 0x36);
    CHECK(copied_host->data[1][0] == 0x63);

    av_buffer_unref(&frames_ref);
    return result;
  }();

  const auto host = cached.ToHost();
  const auto& view = host.view();
  CHECK(view.buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(view.buffer.pixel_format == kMwStreamerVideoPixelFormatNv12);
  CHECK(view.timestamp.pts == 9);
  REQUIRE(view.buffer.storage.linear.plane_count == 2);
  const auto* y = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[0].address);
  const auto* uv = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[1].address);
  CHECK(y[0] == 0x36);
  CHECK(y[kWidth * kHeight - 1] == 0x36);
  CHECK(uv[0] == 0x63);
  CHECK(uv[kWidth * (kHeight / 2) - 1] == 0x63);
}

TEST_CASE("CudaFrame拒绝非linear和无效平面") {
  MwStreamerVideoFrameView source{};
  source.buffer.memory_type = kMwStreamerMemoryHost;
  source.buffer.storage_type = kMwStreamerVideoStorageNativeSurface;
  source.buffer.pixel_format = kMwStreamerVideoPixelFormatNv12;
  source.buffer.width = 4;
  source.buffer.height = 4;
  CHECK_THROWS_AS(CudaFrame::CopyFrom(source), std::invalid_argument);

  source.buffer.storage_type = kMwStreamerVideoStorageLinear;
  source.buffer.storage.linear = {nullptr, 0};
  CHECK_THROWS_AS(CudaFrame::CopyFrom(source), std::invalid_argument);
}

TEST_CASE("CudaFrame复制三平面16位帧并校验CopyTo目标") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  constexpr std::uint32_t kWidth = 8;
  constexpr std::uint32_t kHeight = 4;
  constexpr std::uint32_t kRowBytes = kWidth * 2;
  std::array<std::vector<std::uint16_t>, 3> storage;
  std::array<MwStreamerVideoPlaneView, 3> source_planes;
  for (std::uint32_t plane = 0; plane < source_planes.size(); ++plane) {
    storage[plane].assign(kWidth * kHeight,
                          static_cast<std::uint16_t>(0x1111U * (plane + 1)));
    source_planes[plane] = {
        reinterpret_cast<std::uintptr_t>(storage[plane].data()), kRowBytes,
        kRowBytes, kHeight};
  }
  const MwStreamerVideoFrameView source = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatYuv444p16le,
       kWidth,
       kHeight,
       {.linear = {source_planes.data(),
                   static_cast<std::uint32_t>(source_planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };
  const auto frame = CudaFrame::CopyFrom(source);

  constexpr std::uint32_t kOutputStride = kRowBytes + 8;
  std::array<std::vector<std::uint8_t>, 3> output_storage;
  std::array<MwStreamerVideoPlaneView, 3> output_planes;
  for (std::uint32_t plane = 0; plane < output_planes.size(); ++plane) {
    output_storage[plane].assign(kOutputStride * kHeight, 0xee);
    output_planes[plane] = {
        reinterpret_cast<std::uintptr_t>(output_storage[plane].data()),
        kOutputStride, kRowBytes, kHeight};
  }
  auto destination = source.buffer;
  destination.storage.linear = {
      output_planes.data(), static_cast<std::uint32_t>(output_planes.size())};
  CHECK_NOTHROW(frame.CopyTo(destination));
  for (std::uint32_t plane = 0; plane < output_planes.size(); ++plane) {
    const auto* first_value =
        reinterpret_cast<const std::uint16_t*>(output_storage[plane].data());
    CHECK(first_value[0] == static_cast<std::uint16_t>(0x1111U * (plane + 1)));
    CHECK(output_storage[plane][kRowBytes] == 0xee);
  }

  const auto check_rejected = [&](const MwStreamerVideoBufferView& invalid) {
    CHECK_THROWS_AS(frame.CopyTo(invalid), std::invalid_argument);
  };
  auto invalid = destination;
  invalid.memory_type = static_cast<MwStreamerMemoryType>(999);
  check_rejected(invalid);
  invalid = destination;
  invalid.storage_type = kMwStreamerVideoStorageNativeSurface;
  check_rejected(invalid);
  invalid = destination;
  invalid.pixel_format = kMwStreamerVideoPixelFormatYuv444p;
  check_rejected(invalid);
  invalid = destination;
  invalid.width -= 1;
  check_rejected(invalid);
  invalid = destination;
  invalid.height -= 1;
  check_rejected(invalid);
  invalid = destination;
  invalid.storage.linear = {nullptr, 0};
  check_rejected(invalid);
  invalid = destination;
  invalid.storage.linear.plane_count = 2;
  check_rejected(invalid);

  invalid = destination;
  output_planes[2].address = 0;
  check_rejected(invalid);
  output_planes[2] = source_planes[2];
  output_planes[2].stride_bytes = 1;
  check_rejected(invalid);
  output_planes[2] = source_planes[2];
  output_planes[2].row_bytes -= 2;
  check_rejected(invalid);
  output_planes[2] = source_planes[2];
  output_planes[2].row_count -= 1;
  check_rejected(invalid);
  output_planes[2] = source_planes[2];
  invalid.memory_type = kMwStreamerMemoryCuda;
  output_planes[2].stride_bytes = -output_planes[2].stride_bytes;
  check_rejected(invalid);
}

}  // namespace

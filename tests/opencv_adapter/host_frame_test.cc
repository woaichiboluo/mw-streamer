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
using mw::streamer::opencv_adapter::HostFrame;
using mw::streamer::processor::internal::VideoFrameAdapter;

static_assert(!std::is_copy_constructible_v<HostFrame>);
static_assert(!std::is_copy_assignable_v<HostFrame>);
static_assert(std::is_nothrow_move_constructible_v<HostFrame>);
static_assert(std::is_nothrow_move_assignable_v<HostFrame>);

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

MwStreamerMediaTimestamp MakeTimestamp() { return {1234, 40, {1, 1000}}; }

TEST_CASE("HostFrame深拷贝带padding和负stride的Host帧") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  constexpr std::int32_t kStride = 6;
  std::vector<std::uint8_t> y(kStride * kHeight, 0xee);
  std::vector<std::uint8_t> uv(kStride * (kHeight / 2), 0xdd);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    for (std::uint32_t column = 0; column < kWidth; ++column) {
      y[(kHeight - 1 - row) * kStride + column] =
          static_cast<std::uint8_t>(row * 10 + column);
    }
  }
  for (std::uint32_t row = 0; row < kHeight / 2; ++row) {
    for (std::uint32_t column = 0; column < kWidth; ++column) {
      uv[row * kStride + column] =
          static_cast<std::uint8_t>(100 + row * 10 + column);
    }
  }

  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data() + (kHeight - 1) * kStride),
       -kStride, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kStride, kWidth,
       kHeight / 2},
  }};
  const MwStreamerVideoFrameView source = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatNv12,
       kWidth,
       kHeight,
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };

  auto frame = HostFrame::CopyFrom(source);
  std::memset(y.data(), 0, y.size());
  std::memset(uv.data(), 0, uv.size());

  const auto& view = frame.view();
  CHECK(view.buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(view.buffer.pixel_format == kMwStreamerVideoPixelFormatNv12);
  CHECK(view.buffer.width == kWidth);
  CHECK(view.buffer.height == kHeight);
  CHECK(view.color.space == kMwStreamerColorSpaceBt709);
  CHECK(view.timestamp.pts == 1234);
  REQUIRE(view.buffer.storage.linear.plane_count == 2);
  for (std::uint32_t index = 0; index < 2; ++index) {
    const auto& plane = view.buffer.storage.linear.planes[index];
    CHECK(plane.stride_bytes == static_cast<std::int32_t>(plane.row_bytes));
  }

  const auto* copied_y = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[0].address);
  const auto* copied_uv = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[1].address);
  CHECK(copied_y[0] == 0);
  CHECK(copied_y[3 * kWidth + 3] == 33);
  CHECK(copied_uv[0] == 100);
  CHECK(copied_uv[kWidth + 3] == 113);

  std::vector<std::uint8_t> output_y(kStride * kHeight, 0xff);
  std::vector<std::uint8_t> output_uv(kStride * (kHeight / 2), 0xff);
  const std::array<MwStreamerVideoPlaneView, 2> output_planes = {{
      {reinterpret_cast<std::uintptr_t>(output_y.data() +
                                        (kHeight - 1) * kStride),
       -kStride, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(output_uv.data()), kStride, kWidth,
       kHeight / 2},
  }};
  auto host_output = source.buffer;
  host_output.storage.linear = {
      output_planes.data(), static_cast<std::uint32_t>(output_planes.size())};
  frame.CopyTo(host_output);
  CHECK(output_y[(kHeight - 1) * kStride] == 0);
  CHECK(output_y[3] == 33);
  CHECK(output_uv[0] == 100);
  CHECK(output_uv[kStride + 3] == 113);

  auto cuda_output = CudaFrame::CopyFrom(source);
  frame.CopyTo(cuda_output.view().buffer);
  const auto copied_back = cuda_output.ToHost();
  const auto& copied_back_linear = copied_back.view().buffer.storage.linear;
  const auto* cuda_y = reinterpret_cast<const std::uint8_t*>(
      copied_back_linear.planes[0].address);
  const auto* cuda_uv = reinterpret_cast<const std::uint8_t*>(
      copied_back_linear.planes[1].address);
  CHECK(cuda_y[0] == 0);
  CHECK(cuda_y[3 * kWidth + 3] == 33);
  CHECK(cuda_uv[0] == 100);
  CHECK(cuda_uv[kWidth + 3] == 113);

  auto moved = std::move(frame);
  CHECK(moved.view().buffer.storage.linear.planes[0].address ==
        reinterpret_cast<std::uintptr_t>(copied_y));
}

TEST_CASE("HostFrame同步下载CUDA帧并脱离源帧生命周期") {
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
    std::memset(source->data[0], 0x5a,
                static_cast<std::size_t>(source->linesize[0]) * kHeight);
    std::memset(source->data[1], 0xa5,
                static_cast<std::size_t>(source->linesize[1]) * (kHeight / 2));

    Frame cuda;
    ThrowIfError(av_hwframe_get_buffer(frames_ref, cuda.get(), 0),
                 "分配测试CUDA帧");
    av_buffer_unref(&frames_ref);
    ThrowIfError(av_hwframe_transfer_data(cuda.get(), source.get(), 0),
                 "上传测试CUDA帧");
    cuda->time_base = {1, 25};
    cuda->pts = 7;
    cuda->duration = 1;
    cuda->colorspace = AVCOL_SPC_BT709;

    const VideoFrameAdapter adapter(cuda);
    REQUIRE(adapter.view().buffer.memory_type == kMwStreamerMemoryCuda);
    return HostFrame::CopyFrom(adapter.view());
  }();

  const auto& view = cached.view();
  CHECK(view.buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(view.buffer.pixel_format == kMwStreamerVideoPixelFormatNv12);
  CHECK(view.timestamp.pts == 7);
  REQUIRE(view.buffer.storage.linear.plane_count == 2);
  const auto* y = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[0].address);
  const auto* uv = reinterpret_cast<const std::uint8_t*>(
      view.buffer.storage.linear.planes[1].address);
  CHECK(y[0] == 0x5a);
  CHECK(y[kWidth * kHeight - 1] == 0x5a);
  CHECK(uv[0] == 0xa5);
  CHECK(uv[kWidth * (kHeight / 2) - 1] == 0xa5);
}

TEST_CASE("HostFrame拒绝非linear和无效平面") {
  MwStreamerVideoFrameView source{};
  source.buffer.memory_type = kMwStreamerMemoryHost;
  source.buffer.storage_type = kMwStreamerVideoStorageNativeSurface;
  source.buffer.pixel_format = kMwStreamerVideoPixelFormatNv12;
  source.buffer.width = 4;
  source.buffer.height = 4;
  CHECK_THROWS_AS(HostFrame::CopyFrom(source), std::invalid_argument);

  source.buffer.storage_type = kMwStreamerVideoStorageLinear;
  source.buffer.storage.linear = {nullptr, 0};
  CHECK_THROWS_AS(HostFrame::CopyFrom(source), std::invalid_argument);
}

TEST_CASE("HostFrame CopyTo校验目标格式和全部平面布局") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint8_t> y(kWidth * kHeight, 0x31);
  std::vector<std::uint8_t> uv(kWidth * kHeight / 2, 0x72);
  const std::array<MwStreamerVideoPlaneView, 2> source_planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data()), kWidth, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kWidth, kWidth,
       kHeight / 2},
  }};
  const MwStreamerVideoFrameView source = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatNv12,
       kWidth,
       kHeight,
       {.linear = {source_planes.data(),
                   static_cast<std::uint32_t>(source_planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };
  const auto frame = HostFrame::CopyFrom(source);

  std::vector<std::uint8_t> output_y((kWidth + 2) * kHeight, 0xff);
  std::vector<std::uint8_t> output_uv((kWidth + 2) * (kHeight / 2), 0xff);
  std::array<MwStreamerVideoPlaneView, 2> output_planes = {{
      {reinterpret_cast<std::uintptr_t>(output_y.data()), kWidth + 2, kWidth,
       kHeight},
      {reinterpret_cast<std::uintptr_t>(output_uv.data()), kWidth + 2, kWidth,
       kHeight / 2},
  }};
  auto destination = source.buffer;
  destination.storage.linear = {
      output_planes.data(), static_cast<std::uint32_t>(output_planes.size())};
  CHECK_NOTHROW(frame.CopyTo(destination));
  CHECK(output_y[0] == 0x31);
  CHECK(output_y[kWidth] == 0xff);
  CHECK(output_uv[0] == 0x72);
  CHECK(output_uv[kWidth] == 0xff);

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
  invalid.pixel_format = kMwStreamerVideoPixelFormatP010;
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
  invalid.storage.linear.plane_count = 1;
  check_rejected(invalid);

  invalid = destination;
  output_planes[0].address = 0;
  check_rejected(invalid);
  output_planes[0] = source_planes[0];
  output_planes[0].stride_bytes = 0;
  check_rejected(invalid);
  output_planes[0] = source_planes[0];
  output_planes[0].row_bytes -= 1;
  check_rejected(invalid);
  output_planes[0] = source_planes[0];
  output_planes[0].row_count -= 1;
  check_rejected(invalid);
  output_planes[0] = source_planes[0];
  invalid.memory_type = kMwStreamerMemoryCuda;
  output_planes[0].stride_bytes = -output_planes[0].stride_bytes;
  check_rejected(invalid);
}

}  // namespace

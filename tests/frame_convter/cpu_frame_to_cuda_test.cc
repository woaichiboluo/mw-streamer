#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <stdexcept>

#include "mw/frame_convter/opencv_cuda_convter.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace {

using mw::frame_convter::OpencvCudaConvter;
namespace ffmpeg = mw::streamer::ffmpeg;

ffmpeg::Frame SoftwareFrame(AVPixelFormat format) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->width = 34;
  frame->height = 18;
  frame->pts = 7654;
  frame->time_base = {1, 1000};
  frame->duration = 40;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_range = AVCOL_RANGE_MPEG;
  REQUIRE(av_frame_get_buffer(frame.get(), 64) == 0);
  for (int plane = 0; plane < 2; ++plane) {
    const int height = plane == 0 ? frame->height : frame->height / 2;
    const int bytes = frame->width * (format == AV_PIX_FMT_P010LE ? 2 : 1);
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < bytes; ++column) {
        frame->data[plane][row * frame->linesize[plane] + column] =
            static_cast<std::uint8_t>((row * 13 + column * 7 + plane * 31) %
                                      256);
      }
    }
  }
  return frame;
}

TEST_CASE("CUDA frame upload rejects a CPU destination device",
          "[frame-convter][cpu]") {
  auto frame = SoftwareFrame(AV_PIX_FMT_NV12);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  REQUIRE_THROWS_AS(OpencvCudaConvter::CpuFrameToCuda(frame, cpu),
                    std::invalid_argument);
}

TEST_CASE("CUDA frame upload preserves software layout pixels and properties",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  auto source = SoftwareFrame(format);
  ffmpeg::HwDeviceContext cuda(ffmpeg::HwDeviceType::kCuda);
  auto uploaded = OpencvCudaConvter::CpuFrameToCuda(source, cuda);
  REQUIRE(uploaded->format == AV_PIX_FMT_CUDA);
  REQUIRE(uploaded->hw_frames_ctx != nullptr);
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(uploaded->hw_frames_ctx->data);
  REQUIRE(pool->sw_format == format);
  REQUIRE(pool->device_ref->data == cuda.get()->data);
  REQUIRE(uploaded->width == source->width);
  REQUIRE(uploaded->height == source->height);
  REQUIRE(uploaded->pts == source->pts);
  REQUIRE(uploaded->duration == source->duration);
  REQUIRE(uploaded->time_base.num == source->time_base.num);
  REQUIRE(uploaded->time_base.den == source->time_base.den);
  REQUIRE(uploaded->colorspace == source->colorspace);
  REQUIRE(uploaded->color_range == source->color_range);
  ffmpeg::Frame downloaded;
  REQUIRE(av_hwframe_transfer_data(downloaded.get(), uploaded.get(), 0) >= 0);
  REQUIRE(downloaded->format == format);
  for (int plane = 0; plane < 2; ++plane) {
    const int height = plane == 0 ? source->height : source->height / 2;
    const int bytes = source->width * (format == AV_PIX_FMT_P010LE ? 2 : 1);
    for (int row = 0; row < height; ++row) {
      CAPTURE(format, plane, row);
      REQUIRE(std::equal(
          source->data[plane] + row * source->linesize[plane],
          source->data[plane] + row * source->linesize[plane] + bytes,
          downloaded->data[plane] + row * downloaded->linesize[plane]));
    }
  }
  REQUIRE_THROWS_AS(OpencvCudaConvter::CpuFrameToCuda(uploaded, cuda),
                    std::invalid_argument);
  auto unsupported = SoftwareFrame(AV_PIX_FMT_NV12);
  unsupported->format = AV_PIX_FMT_PAL8;
  REQUIRE_THROWS_AS(OpencvCudaConvter::CpuFrameToCuda(unsupported, cuda),
                    std::invalid_argument);
  auto negative_stride = SoftwareFrame(format);
  negative_stride->data[0] +=
      (negative_stride->height - 1) * negative_stride->linesize[0];
  negative_stride->linesize[0] = -negative_stride->linesize[0];
  REQUIRE_THROWS_AS(OpencvCudaConvter::CpuFrameToCuda(negative_stride, cuda),
                    std::invalid_argument);
  auto odd_size = SoftwareFrame(format);
  --odd_size->width;
  REQUIRE_THROWS_AS(OpencvCudaConvter::CpuFrameToCuda(odd_size, cuda),
                    std::invalid_argument);
}

TEST_CASE("CUDA uploaded frames retain their device and pixel ownership",
          "[.][frame-convter][cuda]") {
  auto uploaded = [] {
    auto source = SoftwareFrame(AV_PIX_FMT_NV12);
    ffmpeg::HwDeviceContext cuda(ffmpeg::HwDeviceType::kCuda);
    return OpencvCudaConvter::CpuFrameToCuda(source, cuda);
  }();
  ffmpeg::Frame downloaded;
  REQUIRE(av_hwframe_transfer_data(downloaded.get(), uploaded.get(), 0) >= 0);
  const auto expected = SoftwareFrame(AV_PIX_FMT_NV12);
  REQUIRE(downloaded->format == expected->format);
  for (int plane = 0; plane < 2; ++plane) {
    const int height = plane == 0 ? expected->height : expected->height / 2;
    for (int row = 0; row < height; ++row) {
      CAPTURE(plane, row);
      REQUIRE(std::equal(
          expected->data[plane] + row * expected->linesize[plane],
          expected->data[plane] + row * expected->linesize[plane] +
              expected->width,
          downloaded->data[plane] + row * downloaded->linesize[plane]));
    }
  }
}

}  // namespace

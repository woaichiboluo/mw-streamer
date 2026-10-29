#include <cuda.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <stdexcept>

#include "mw/frame_convter/opencv_cuda_convter.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/pixdesc.h>
}

namespace {

using mw::frame_convter::OpencvCudaConvter;
namespace ffmpeg = mw::streamer::ffmpeg;

void WaitForUpload(const ffmpeg::Frame& frame) {
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  const auto* device =
      static_cast<const AVCUDADeviceContext*>(pool->device_ctx->hwctx);
  REQUIRE(cuCtxPushCurrent(device->cuda_ctx) == CUDA_SUCCESS);
  const auto completed = cuStreamSynchronize(nullptr);
  CUcontext previous = nullptr;
  const auto restored = cuCtxPopCurrent(&previous);
  REQUIRE(completed == CUDA_SUCCESS);
  REQUIRE(restored == CUDA_SUCCESS);
}

int PlaneHeight(const ffmpeg::Frame& frame, int plane) {
  return plane == 0 || plane == 3 || frame->format == AV_PIX_FMT_YUV444P
             ? frame->height
             : frame->height / 2;
}

int PlaneBytes(const ffmpeg::Frame& frame, int plane) {
  if (frame->format == AV_PIX_FMT_P010LE) return frame->width * 2;
  return (frame->format == AV_PIX_FMT_YUV420P ||
          frame->format == AV_PIX_FMT_YUVA420P) &&
                 (plane == 1 || plane == 2)
             ? frame->width / 2
             : frame->width;
}

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
  for (int plane = 0; plane < av_pix_fmt_count_planes(format); ++plane) {
    const int height = PlaneHeight(frame, plane);
    const int bytes = PlaneBytes(frame, plane);
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
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  CUcontext before = nullptr;
  const auto before_status = cuCtxGetCurrent(&before);
  REQUIRE_THROWS_AS(OpencvCudaConvter(cpu), std::invalid_argument);
  CUcontext after = nullptr;
  REQUIRE(cuCtxGetCurrent(&after) == before_status);
  REQUIRE(after == before);
}

TEST_CASE("CUDA frame upload preserves software layout pixels and properties",
          "[.][frame-convter][cuda]") {
  const auto format =
      GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P,
               AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVA420P);
  auto source = SoftwareFrame(format);
  ffmpeg::HwDeviceContext cuda(ffmpeg::HwDeviceType::kCuda);
  OpencvCudaConvter converter(cuda, 0);
  if (format == AV_PIX_FMT_YUVA420P) {
    auto* constraints =
        av_hwdevice_get_hwframe_constraints(cuda.get(), nullptr);
    REQUIRE(constraints != nullptr);
    bool supported = false;
    if (constraints->valid_sw_formats) {
      for (const auto* supported_format = constraints->valid_sw_formats;
           *supported_format != AV_PIX_FMT_NONE; ++supported_format) {
        supported = supported || *supported_format == format;
      }
    }
    av_hwframe_constraints_free(&constraints);
    if (!supported) SKIP("当前 FFmpeg CUDA 不支持四平面 YUVA420P 上传");
  }
  auto uploaded = converter.CpuFrameToCuda(source);
  WaitForUpload(uploaded);
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
  for (int plane = 0; plane < av_pix_fmt_count_planes(format); ++plane) {
    const int height = PlaneHeight(source, plane);
    const int bytes = PlaneBytes(source, plane);
    for (int row = 0; row < height; ++row) {
      CAPTURE(format, plane, row);
      REQUIRE(std::equal(
          source->data[plane] + row * source->linesize[plane],
          source->data[plane] + row * source->linesize[plane] + bytes,
          downloaded->data[plane] + row * downloaded->linesize[plane]));
    }
  }
  REQUIRE_THROWS_AS(converter.CpuFrameToCuda(uploaded), std::invalid_argument);
  auto unsupported = SoftwareFrame(AV_PIX_FMT_NV12);
  unsupported->format = AV_PIX_FMT_PAL8;
  REQUIRE_THROWS_AS(converter.CpuFrameToCuda(unsupported),
                    std::invalid_argument);
  auto negative_stride = SoftwareFrame(format);
  negative_stride->data[0] +=
      (negative_stride->height - 1) * negative_stride->linesize[0];
  negative_stride->linesize[0] = -negative_stride->linesize[0];
  REQUIRE_THROWS_AS(converter.CpuFrameToCuda(negative_stride),
                    std::invalid_argument);
  auto odd_size = SoftwareFrame(format);
  --odd_size->width;
  if (format == AV_PIX_FMT_YUV444P) {
    auto odd_uploaded = converter.CpuFrameToCuda(odd_size);
    WaitForUpload(odd_uploaded);
    REQUIRE(odd_uploaded->width == odd_size->width);
  } else {
    REQUIRE_THROWS_AS(converter.CpuFrameToCuda(odd_size),
                      std::invalid_argument);
  }
}

TEST_CASE("CUDA uploaded frames retain their device and pixel ownership",
          "[.][frame-convter][cuda]") {
  auto uploaded = [] {
    auto source = SoftwareFrame(AV_PIX_FMT_NV12);
    ffmpeg::HwDeviceContext cuda(ffmpeg::HwDeviceType::kCuda);
    OpencvCudaConvter converter(cuda, 0);
    auto result = converter.CpuFrameToCuda(source);
    WaitForUpload(result);
    return result;
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

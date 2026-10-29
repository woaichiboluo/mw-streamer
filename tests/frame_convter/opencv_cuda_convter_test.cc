#include "mw/frame_convter/opencv_cuda_convter.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/filter/av_media_filter.h"

namespace {

using mw::frame_convter::OpencvCudaConvter;
using mw::frame_convter::RgbOrder;
namespace ffmpeg = mw::streamer::ffmpeg;

class TestContextBinding final {
 public:
  explicit TestContextBinding(CUcontext context) {
    REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
  }
  ~TestContextBinding() {
    CUcontext previous = nullptr;
    if (cuCtxPopCurrent(&previous) != CUDA_SUCCESS) {
      std::fputs("Failed to restore CUDA test context\n", stderr);
    }
  }
  TestContextBinding(const TestContextBinding&) = delete;
  TestContextBinding& operator=(const TestContextBinding&) = delete;
};

class TestStream final {
 public:
  TestStream() {
    REQUIRE(cuCtxGetCurrent(&context_) == CUDA_SUCCESS);
    REQUIRE(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) ==
            cudaSuccess);
  }
  ~TestStream() {
    if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) {
      std::fputs("Failed to bind CUDA test stream context\n", stderr);
      return;
    }
    if (cudaStreamSynchronize(stream_) != cudaSuccess) {
      std::fputs("Failed to finish CUDA test stream\n", stderr);
    }
    if (cudaStreamDestroy(stream_) != cudaSuccess) {
      std::fputs("Failed to destroy CUDA test stream\n", stderr);
    }
    CUcontext previous = nullptr;
    if (cuCtxPopCurrent(&previous) != CUDA_SUCCESS) {
      std::fputs("Failed to restore CUDA test stream context\n", stderr);
    }
  }
  TestStream(const TestStream&) = delete;
  TestStream& operator=(const TestStream&) = delete;
  cudaStream_t get() const { return stream_; }

 private:
  CUcontext context_ = nullptr;
  cudaStream_t stream_ = nullptr;
};

void WaitForFrame(const ffmpeg::Frame& frame) {
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

ffmpeg::HwDeviceContext PrimaryDevice() {
  cv::cuda::setDevice(0);
  AVDictionary* options = nullptr;
  REQUIRE(av_dict_set(&options, "current_ctx", "1", 0) == 0);
  try {
    ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda, "0", options);
    av_dict_free(&options);
    return device;
  } catch (...) {
    av_dict_free(&options);
    throw;
  }
}

ffmpeg::Frame SoftwareSample(AVPixelFormat format, AVColorSpace matrix,
                             AVColorRange range, int y, int u, int v,
                             int width = 34, int height = 18) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->width = width;
  frame->height = height;
  frame->colorspace = matrix;
  frame->color_range = range;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  frame->pts = 12345;
  frame->duration = 3000;
  frame->time_base = {1, 90000};
  frame->sample_aspect_ratio = {4, 3};
  REQUIRE(av_dict_set(&frame->metadata, "source", "cuda-test-camera", 0) == 0);
  REQUIRE(av_frame_get_buffer(frame.get(), 64) == 0);
  for (int plane = 0; plane < 2; ++plane) {
    const int rows = plane == 0 ? frame->height : frame->height / 2;
    for (int row = 0; row < rows; ++row) {
      auto* pixels = frame->data[plane] + row * frame->linesize[plane];
      for (int column = 0; column < frame->width; ++column) {
        const int value = plane == 0 ? y : (column % 2 == 0 ? u : v);
        if (format == AV_PIX_FMT_P010LE) {
          reinterpret_cast<std::uint16_t*>(pixels)[column] =
              static_cast<std::uint16_t>(value * 4 << 6);
        } else {
          pixels[column] = static_cast<std::uint8_t>(value);
        }
      }
    }
  }
  return frame;
}

void CheckPixels(const cv::cuda::GpuMat& image, const cv::Vec3b& expected,
                 int tolerance = 3) {
  REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
  cv::Mat downloaded;
  image.download(downloaded);
  REQUIRE(downloaded.type() == CV_8UC3);
  for (int row = 0; row < downloaded.rows; ++row) {
    for (int column = 0; column < downloaded.cols; ++column) {
      const auto actual = downloaded.at<cv::Vec3b>(row, column);
      for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(row, column, channel);
        REQUIRE(std::abs(static_cast<int>(actual[channel]) -
                         expected[channel]) <= tolerance);
      }
    }
  }
}

ffmpeg::Frame Download(const ffmpeg::Frame& source) {
  WaitForFrame(source);
  ffmpeg::Frame result;
  REQUIRE(av_hwframe_transfer_data(result.get(), source.get(), 0) >= 0);
  return result;
}

void ZeroPixels(const ffmpeg::Frame& frame) {
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  const auto* cuda =
      static_cast<const AVCUDADeviceContext*>(pool->device_ctx->hwctx);
  TestContextBinding binding(cuda->cuda_ctx);
  const auto bytes = static_cast<std::size_t>(frame->width) *
                     (pool->sw_format == AV_PIX_FMT_P010LE ? 2U : 1U);
  for (int plane = 0; plane < 2; ++plane) {
    const auto rows = static_cast<std::size_t>(plane == 0 ? frame->height
                                                          : frame->height / 2);
    REQUIRE(cudaMemset2DAsync(frame->data[plane], frame->linesize[plane], 0,
                              bytes, rows, nullptr) == cudaSuccess);
  }
  REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
}

void CheckYuv(const ffmpeg::Frame& frame, int y, int u, int v,
              int tolerance = 2) {
  const bool p010 = frame->format == AV_PIX_FMT_P010LE;
  for (int plane = 0; plane < 2; ++plane) {
    const int rows = plane == 0 ? frame->height : frame->height / 2;
    for (int row = 0; row < rows; ++row) {
      const auto* pixels = frame->data[plane] + row * frame->linesize[plane];
      for (int column = 0; column < frame->width; ++column) {
        const int expected = plane == 0 ? y : (column % 2 == 0 ? u : v);
        int actual = pixels[column];
        if (p010) {
          const auto value =
              reinterpret_cast<const std::uint16_t*>(pixels)[column];
          REQUIRE((value & 63) == 0);
          actual = value >> 6;
        }
        CAPTURE(plane, row, column, p010);
        REQUIRE(std::abs(actual - expected * (p010 ? 4 : 1)) <=
                tolerance * (p010 ? 4 : 1));
      }
    }
  }
}

TEST_CASE("CUDA conversion honors NV12 and P010 matrix range and RGB order",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const auto matrix =
      GENERATE(AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709, AVCOL_SPC_UNSPECIFIED);
  const auto range = GENERATE(AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG);
  const bool limited = range == AVCOL_RANGE_MPEG;
  const bool bt709 = matrix != AVCOL_SPC_SMPTE170M;
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  SECTION("black and white honor native range") {
    const bool white = GENERATE(false, true);
    const int y = limited ? (white ? 235 : 16) : (white ? 255 : 0);
    auto software = SoftwareSample(format, matrix, range, y, 128, 128);
    auto frame = converter.CpuFrameToCuda(software);
    auto bgr = converter.ToMat(frame);
    REQUIRE(bgr.rows == software->height);
    REQUIRE(bgr.cols == software->width);
    const auto expected = static_cast<std::uint8_t>(white ? 255 : 0);
    CheckPixels(bgr, cv::Vec3b(expected, expected, expected));
  }
  SECTION("red uses native matrix with explicit and default channel order") {
    const int y = limited ? (bt709 ? 63 : 81) : (bt709 ? 54 : 76);
    const int u = limited ? (bt709 ? 102 : 90) : (bt709 ? 99 : 85);
    const int v = limited ? 240 : 255;
    auto software = SoftwareSample(format, matrix, range, y, u, v);
    auto frame = converter.CpuFrameToCuda(software);
    auto bgr = converter.ToMat(frame);
    auto rgb = converter.ToMat(frame, RgbOrder::kRgb);
    REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
    frame.Unref();
    CheckPixels(bgr, cv::Vec3b(0, 0, 255));
    CheckPixels(rgb, cv::Vec3b(255, 0, 0));
  }
}

TEST_CASE("CUDA reverse conversion owns pixels and inherits reference metadata",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const auto matrix =
      GENERATE(AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709, AVCOL_SPC_UNSPECIFIED);
  const auto range = GENERATE(AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG);
  const auto order = GENERATE(RgbOrder::kBgr, RgbOrder::kRgb);
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(format, matrix, range, 128, 128, 128);
  auto reference = converter.CpuFrameToCuda(software);
  reference->crop_left = 2;
  reference->crop_right = 2;
  cv::Mat input(
      16, 28, CV_8UC3,
      order == RgbOrder::kBgr ? cv::Scalar(0, 0, 255) : cv::Scalar(255, 0, 0));
  cv::cuda::GpuMat storage;
  storage.upload(input);
  auto image = storage(cv::Rect(3, 2, 20, 12));
  REQUIRE_FALSE(image.isContinuous());
  auto frame = converter.ToFrame(image, reference, order);
  REQUIRE(frame->format == AV_PIX_FMT_CUDA);
  REQUIRE(frame->width == 20);
  REQUIRE(frame->height == 12);
  REQUIRE(frame->hw_frames_ctx != nullptr);
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  REQUIRE(pool->sw_format == format);
  REQUIRE(pool->device_ref->data == device.get()->data);
  REQUIRE(frame->pts == reference->pts);
  REQUIRE(frame->duration == reference->duration);
  REQUIRE(frame->time_base.num == reference->time_base.num);
  REQUIRE(frame->time_base.den == reference->time_base.den);
  REQUIRE(frame->sample_aspect_ratio.num == 4);
  REQUIRE(frame->sample_aspect_ratio.den == 3);
  REQUIRE(frame->colorspace == matrix);
  REQUIRE(frame->color_range == range);
  REQUIRE(frame->color_trc == reference->color_trc);
  REQUIRE(frame->color_primaries == reference->color_primaries);
  REQUIRE(frame->chroma_location == reference->chroma_location);
  REQUIRE(frame->crop_left == 0);
  REQUIRE(frame->crop_right == 0);
  const auto* entry = av_dict_get(frame->metadata, "source", nullptr, 0);
  REQUIRE(entry != nullptr);
  REQUIRE(std::string(entry->value) == "cuda-test-camera");
  WaitForFrame(frame);
  storage.setTo(cv::Scalar(0, 0, 0));
  reference.Unref();
  const bool limited = range == AVCOL_RANGE_MPEG;
  const bool bt709 = matrix != AVCOL_SPC_SMPTE170M;
  const int y = limited ? (bt709 ? 63 : 81) : (bt709 ? 54 : 76);
  const int u = limited ? (bt709 ? 102 : 90) : (bt709 ? 99 : 85);
  CheckYuv(Download(frame), y, u, limited ? 240 : 255);
  CheckPixels(converter.ToMat(frame), cv::Vec3b(0, 0, 255), 5);
}

TEST_CASE("CUDA reverse conversion defaults to OpenCV BGR order",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto reference = converter.CpuFrameToCuda(software);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  auto frame = converter.ToFrame(image, reference);
  CheckYuv(Download(frame), 63, 102, 240);
}

TEST_CASE(
    "CUDA conversion rejects unsupported frames images and color metadata",
    "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto frame = converter.CpuFrameToCuda(software);
  WaitForFrame(frame);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 0));
  cv::cuda::GpuMat image;
  image.upload(host);
  SECTION("CPU frame") {
    REQUIRE_THROWS_AS(converter.ToMat(software), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, software),
                      std::invalid_argument);
  }
  SECTION("missing hardware pool") {
    av_buffer_unref(&frame->hw_frames_ctx);
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  }
  SECTION("unsupported matrix") {
    frame->colorspace = AVCOL_SPC_BT2020_CL;
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  }
  SECTION("missing chroma location") {
    frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  }
  SECTION("missing range") {
    frame->color_range = AVCOL_RANGE_UNSPECIFIED;
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  }
  SECTION("HDR requires explicit tone mapping") {
    frame->color_trc = GENERATE(AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67);
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  }
  SECTION("empty or non-three-channel byte image") {
    REQUIRE_THROWS_AS(converter.ToFrame(cv::cuda::GpuMat{}, frame),
                      std::invalid_argument);
    cv::cuda::GpuMat wrong_type(12, 20, CV_8UC4);
    REQUIRE_THROWS_AS(converter.ToFrame(wrong_type, frame),
                      std::invalid_argument);
  }
  SECTION("odd dimensions") {
    auto odd = image(cv::Rect(0, 0, 19, 12));
    REQUIRE_THROWS_AS(converter.ToFrame(odd, frame), std::invalid_argument);
    --frame->width;
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
  }
  SECTION("invalid RGB order") {
    const auto order = static_cast<RgbOrder>(999);
    REQUIRE_THROWS_AS(converter.ToMat(frame, order), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame, order),
                      std::invalid_argument);
  }
  SECTION("unsupported hardware software format") {
    auto* pool =
        reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    pool->sw_format = AV_PIX_FMT_YUV444P;
    REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
    pool->sw_format = AV_PIX_FMT_NV12;
  }
}

TEST_CASE("CUDA conversion rejects different contexts on the same GPU",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  cv::cuda::setDevice(0);
  CUcontext before = nullptr;
  REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);
  ffmpeg::HwDeviceContext separate(ffmpeg::HwDeviceType::kCuda, "0");
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 63, 102, 240);
  OpencvCudaConvter converter(separate, 0);
  auto frame = converter.CpuFrameToCuda(software);
  WaitForFrame(frame);
  cv::Mat host(18, 34, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
  REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
  CUcontext after = nullptr;
  REQUIRE(cuCtxGetCurrent(&after) == CUDA_SUCCESS);
  REQUIRE(after == before);
}

TEST_CASE("CUDA conversions reject streams belonging to another context",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto frame = converter.CpuFrameToCuda(software);
  auto image = converter.ToMat(frame);
  REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
  CUcontext caller = nullptr;
  REQUIRE(cuCtxGetCurrent(&caller) == CUDA_SUCCESS);
  ffmpeg::HwDeviceContext separate(ffmpeg::HwDeviceType::kCuda, "0");
  const auto* hardware =
      reinterpret_cast<const AVHWDeviceContext*>(separate.get()->data);
  const auto* cuda = static_cast<const AVCUDADeviceContext*>(hardware->hwctx);
  std::optional<TestStream> foreign;
  {
    TestContextBinding binding(cuda->cuda_ctx);
    foreign.emplace();
  }
  REQUIRE_THROWS_AS(converter.ToMat(frame, RgbOrder::kBgr, foreign->get()),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(
      converter.ToFrame(image, frame, RgbOrder::kBgr, foreign->get()),
      std::invalid_argument);
  REQUIRE_THROWS_AS(converter.CpuFrameToCuda(software, foreign->get()),
                    std::invalid_argument);
  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == caller);
}

TEST_CASE("CUDA conversion samples chroma according to native chroma location",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const bool vertical = GENERATE(false, true);
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_JPEG, 128, 128, 128);
  // U changes at chroma column 8 or row 4. Luma position (15,7) lies
  // between those samples; native sample locations change its interpolation.
  for (int row = 0; row < software->height / 2; ++row) {
    auto* pixels = software->data[1] + row * software->linesize[1];
    for (int column = 0; column < software->width / 2; ++column) {
      const int u = (vertical ? row < 4 : column < 8) ? 64 : 192;
      if (format == AV_PIX_FMT_P010LE) {
        reinterpret_cast<std::uint16_t*>(pixels)[2 * column] =
            static_cast<std::uint16_t>(u * 4 << 6);
      } else {
        pixels[2 * column] = static_cast<std::uint8_t>(u);
      }
    }
  }
  auto frame = converter.CpuFrameToCuda(software);
  frame->chroma_location = vertical ? AVCHROMA_LOC_TOP : AVCHROMA_LOC_LEFT;
  cv::Mat first;
  converter.ToMat(frame).download(first);
  frame->chroma_location = vertical ? AVCHROMA_LOC_BOTTOM : AVCHROMA_LOC_CENTER;
  cv::Mat second;
  converter.ToMat(frame).download(second);
  // BT709 full-range blue is Y + 1.8556*(U-128). LEFT/TOP yields
  // U=128 here. P010 CENTER interpolates U=96; NV12 CENTER uses NPP's
  // native nearest upsampling and reads U=64, as does BOTTOM here.
  const int first_blue = first.at<cv::Vec3b>(7, 15)[0];
  const int second_blue = second.at<cv::Vec3b>(7, 15)[0];
  REQUIRE(std::abs(first_blue - 128) <= 3);
  const int expected_second = vertical || format == AV_PIX_FMT_NV12 ? 9 : 69;
  REQUIRE(std::abs(second_blue - expected_second) <= 3);
}

TEST_CASE("CUDA NV12 center conversion averages each RGB two-by-two block",
          "[.][frame-convter][cuda]") {
  const auto matrix =
      GENERATE(AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709, AVCOL_SPC_BT2020_NCL);
  const auto order = GENERATE(RgbOrder::kBgr, RgbOrder::kRgb);
  const double kr = matrix == AVCOL_SPC_SMPTE170M
                        ? 0.299
                        : (matrix == AVCOL_SPC_BT709 ? 0.2126 : 0.2627);
  const double kb = matrix == AVCOL_SPC_SMPTE170M
                        ? 0.114
                        : (matrix == AVCOL_SPC_BT709 ? 0.0722 : 0.0593);
  const double kg = 1.0 - kr - kb;
  const cv::Vec3b bgr[] = {cv::Vec3b(20, 60, 140), cv::Vec3b(80, 100, 220),
                           cv::Vec3b(30, 180, 90), cv::Vec3b(200, 40, 70)};
  double luma[4] = {};
  double u = 0.0;
  double v = 0.0;
  for (int index = 0; index < 4; ++index) {
    luma[index] = kr * bgr[index][2] + kg * bgr[index][1] + kb * bgr[index][0];
    u += 128.0 + (bgr[index][0] - luma[index]) / (2.0 * (1.0 - kb));
    v += 128.0 + (bgr[index][2] - luma[index]) / (2.0 * (1.0 - kr));
  }
  const int expected_u = static_cast<int>(std::lround(u / 4.0));
  const int expected_v = static_cast<int>(std::lround(v / 4.0));
  cv::Mat host(12, 20, CV_8UC3);
  for (int row = 0; row < host.rows; ++row) {
    for (int column = 0; column < host.cols; ++column) {
      auto pixel = bgr[(row % 2) * 2 + column % 2];
      if (order == RgbOrder::kRgb) {
        std::swap(pixel[0], pixel[2]);
      }
      host.at<cv::Vec3b>(row, column) = pixel;
    }
  }
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(AV_PIX_FMT_NV12, matrix, AVCOL_RANGE_JPEG, 128, 128, 128);
  software->chroma_location = AVCHROMA_LOC_CENTER;
  auto reference = converter.CpuFrameToCuda(software);
  cv::cuda::GpuMat image;
  image.upload(host);
  auto output = converter.ToFrame(image, reference, order);
  auto downloaded = Download(output);
  for (int row = 0; row < downloaded->height; ++row) {
    const auto* pixels = downloaded->data[0] + row * downloaded->linesize[0];
    for (int column = 0; column < downloaded->width; ++column) {
      const auto expected =
          static_cast<int>(std::lround(luma[(row % 2) * 2 + column % 2]));
      CAPTURE(matrix, row, column);
      REQUIRE(std::abs(static_cast<int>(pixels[column]) - expected) <= 2);
    }
  }
  for (int row = 0; row < downloaded->height / 2; ++row) {
    const auto* pixels = downloaded->data[1] + row * downloaded->linesize[1];
    for (int column = 0; column < downloaded->width; column += 2) {
      CAPTURE(matrix, row, column);
      REQUIRE(std::abs(static_cast<int>(pixels[column]) - expected_u) <= 2);
      REQUIRE(std::abs(static_cast<int>(pixels[column + 1]) - expected_v) <= 2);
    }
  }
}

TEST_CASE("CUDA conversion accepts all explicit FFmpeg chroma locations",
          "[.][frame-convter][cuda]") {
  const auto location =
      GENERATE(AVCHROMA_LOC_LEFT, AVCHROMA_LOC_CENTER, AVCHROMA_LOC_TOPLEFT,
               AVCHROMA_LOC_TOP, AVCHROMA_LOC_BOTTOMLEFT, AVCHROMA_LOC_BOTTOM);
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 63, 102, 240);
  software->chroma_location = location;
  auto frame = converter.CpuFrameToCuda(software);
  auto image = converter.ToMat(frame);
  CheckPixels(image, cv::Vec3b(0, 0, 255));
  auto output = converter.ToFrame(image, frame);
  REQUIRE(output->chroma_location == location);
  CheckYuv(Download(output), 63, 102, 240);
}

TEST_CASE("CUDA P010 conversion computes ten-bit samples before packing",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_P010LE, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 64, 128, 128);
  auto reference = converter.CpuFrameToCuda(software);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  auto output = converter.ToFrame(image, reference);
  auto downloaded = Download(output);
  // Limited-range BT709 red: Y10 = 64 + 876*0.2126 = 250.2376.
  // Quantizing to NV12 first would give 63*4=252 and lose precision.
  for (int row = 0; row < downloaded->height; ++row) {
    const auto* pixels = reinterpret_cast<const std::uint16_t*>(
        downloaded->data[0] + row * downloaded->linesize[0]);
    for (int column = 0; column < downloaded->width; ++column) {
      REQUIRE((pixels[column] & 63) == 0);
      REQUIRE((pixels[column] >> 6) == 250);
    }
  }
}

TEST_CASE("CUDA conversions share a caller supplied nonblocking stream",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const auto location = GENERATE(AVCHROMA_LOC_LEFT, AVCHROMA_LOC_CENTER);
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  software->chroma_location = location;
  // Declare the stream after all pixel owners so its destructor finishes
  // outstanding work before their storage is released on exceptional exits.
  ffmpeg::Frame uploaded;
  ffmpeg::Frame output;
  cv::cuda::GpuMat bgr;
  cv::Mat downloaded;
  TestStream stream;
  auto opencv_stream = cv::cuda::StreamAccessor::wrapStream(stream.get());
  uploaded = converter.CpuFrameToCuda(software, stream.get());
  bgr = converter.ToMat(uploaded, RgbOrder::kBgr, stream.get());
  bgr.download(downloaded, opencv_stream);
  bgr.setTo(cv::Scalar(0, 0, 255), opencv_stream);
  output = converter.ToFrame(bgr, uploaded, RgbOrder::kBgr, stream.get());
  REQUIRE(cudaStreamSynchronize(stream.get()) == cudaSuccess);
  REQUIRE(downloaded.type() == CV_8UC3);
  REQUIRE(cv::countNonZero(downloaded.reshape(1)) == 0);
  REQUIRE(output->chroma_location == location);
  CheckYuv(Download(output), 63, 102, 240);
}

TEST_CASE("CUDA mat pool reuses blocks after unordered releases",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  constexpr std::size_t kBudget = 6 * 4096;
  OpencvCudaConvter converter(device, kBudget);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128, 64, 16);
  auto frame = converter.CpuFrameToCuda(software);
  std::vector<cv::cuda::GpuMat> images;
  for (int index = 0; index < 6; ++index) {
    images.push_back(converter.ToMat(frame));
    REQUIRE(images.back().allocator != cv::cuda::GpuMat::getStdAllocator());
    REQUIRE(images.back().step * images.back().rows == kBudget / 6);
  }
  REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
  std::vector<std::uint8_t*> released;
  for (const int index : {4, 1, 5, 0, 3, 2}) {
    released.push_back(images[index].data);
    images[index].release();
  }
  for (auto& image : images) {
    image = converter.ToMat(frame);
    const auto previous =
        std::find(released.begin(), released.end(), image.data);
    REQUIRE(previous != released.end());
    released.erase(previous);
  }
  REQUIRE(released.empty());
  for (const auto& image : images) CheckPixels(image, cv::Vec3b(0, 0, 0));
}

TEST_CASE("CUDA mat pool keeps ROI blocks until the final reference releases",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 4096);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128, 64, 16);
  auto frame = converter.CpuFrameToCuda(software);
  auto image = converter.ToMat(frame);
  CheckPixels(image, cv::Vec3b(0, 0, 0));
  auto* original = image.data;
  auto shared = image;
  auto roi = image(cv::Rect(2, 2, 16, 8));
  image.release();
  auto full = converter.ToMat(frame);
  REQUIRE(full.allocator == cv::cuda::GpuMat::getStdAllocator());
  shared.release();
  auto still_full = converter.ToMat(frame);
  REQUIRE(still_full.allocator == cv::cuda::GpuMat::getStdAllocator());
  roi.release();
  auto reused = converter.ToMat(frame);
  REQUIRE(reused.data == original);
  REQUIRE(reused.allocator != cv::cuda::GpuMat::getStdAllocator());
  CheckPixels(full, cv::Vec3b(0, 0, 0));
  CheckPixels(still_full, cv::Vec3b(0, 0, 0));
  CheckPixels(reused, cv::Vec3b(0, 0, 0));
}

TEST_CASE("CUDA default mat pool holds five 7680 by 2160 BGR blocks",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128, 7680, 2160);
  auto frame = converter.CpuFrameToCuda(software);
  std::vector<cv::cuda::GpuMat> images;
  for (int index = 0; index < 6; ++index) {
    images.push_back(converter.ToMat(frame));
    const bool from_pool =
        images.back().allocator != cv::cuda::GpuMat::getStdAllocator();
    REQUIRE(from_pool == (index < 5));
  }
  REQUIRE(cudaStreamSynchronize(nullptr) == cudaSuccess);
  cv::Mat downloaded;
  images.front().download(downloaded);
  REQUIRE(cv::countNonZero(downloaded.reshape(1)) == 0);
}

TEST_CASE("CUDA mat pool falls back without changing its first fitting shape",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 512);
  auto large_software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                       AVCOL_RANGE_MPEG, 16, 128, 128, 64, 16);
  auto large = converter.CpuFrameToCuda(large_software);
  auto oversized = converter.ToMat(large);
  REQUIRE(oversized.allocator == cv::cuda::GpuMat::getStdAllocator());
  auto small_software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                       AVCOL_RANGE_MPEG, 16, 128, 128, 2, 2);
  auto small = converter.CpuFrameToCuda(small_software);
  auto pooled = converter.ToMat(small);
  REQUIRE(pooled.allocator != cv::cuda::GpuMat::getStdAllocator());
  CheckPixels(pooled, cv::Vec3b(0, 0, 0));
  auto* original = pooled.data;
  pooled.release();
  auto changed_software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                         AVCOL_RANGE_MPEG, 16, 128, 128, 4, 2);
  auto changed = converter.CpuFrameToCuda(changed_software);
  auto changed_shape = converter.ToMat(changed);
  REQUIRE(changed_shape.allocator == cv::cuda::GpuMat::getStdAllocator());
  auto reused = converter.ToMat(small);
  REQUIRE(reused.data == original);
  CheckPixels(oversized, cv::Vec3b(0, 0, 0));
  CheckPixels(changed_shape, cv::Vec3b(0, 0, 0));
  CheckPixels(reused, cv::Vec3b(0, 0, 0));
}

TEST_CASE(
    "CUDA conversion reuses frame pools and retains frames across changes",
    "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128, 64, 16);
  auto first = converter.CpuFrameToCuda(software);
  auto second = converter.CpuFrameToCuda(software);
  REQUIRE(first->hw_frames_ctx->data == second->hw_frames_ctx->data);
  REQUIRE(first->data[0] != second->data[0]);
  WaitForFrame(first);
  auto* reusable = first->data[0];
  first.Unref();
  auto third = converter.CpuFrameToCuda(software);
  REQUIRE(third->data[0] == reusable);
  auto bgr = converter.ToMat(second);
  REQUIRE(bgr.allocator == cv::cuda::GpuMat::getStdAllocator());
  auto encoded_first = converter.ToFrame(bgr, second);
  auto encoded_second = converter.ToFrame(bgr, second);
  REQUIRE(encoded_first->hw_frames_ctx->data ==
          encoded_second->hw_frames_ctx->data);
  REQUIRE(encoded_first->data[0] != encoded_second->data[0]);
  auto smaller_image = bgr(cv::Rect(0, 0, 32, 8));
  auto smaller = converter.ToFrame(smaller_image, second);
  REQUIRE(smaller->hw_frames_ctx->data != encoded_first->hw_frames_ctx->data);
  REQUIRE(smaller->width == 32);
  REQUIRE(smaller->height == 8);
  auto ten_bit_software =
      SoftwareSample(AV_PIX_FMT_P010LE, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16,
                     128, 128, 64, 16);
  auto ten_bit = converter.CpuFrameToCuda(ten_bit_software);
  REQUIRE(ten_bit->hw_frames_ctx->data != second->hw_frames_ctx->data);
  auto encoded_ten_bit = converter.ToFrame(bgr, ten_bit);
  REQUIRE(encoded_ten_bit->hw_frames_ctx->data !=
          encoded_first->hw_frames_ctx->data);
  auto smaller_software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                         AVCOL_RANGE_MPEG, 16, 128, 128, 32, 8);
  auto uploaded_smaller = converter.CpuFrameToCuda(smaller_software);
  REQUIRE(uploaded_smaller->hw_frames_ctx->data != second->hw_frames_ctx->data);
  REQUIRE(uploaded_smaller->width == 32);
  REQUIRE(uploaded_smaller->height == 8);
  CheckYuv(Download(second), 16, 128, 128);
  CheckYuv(Download(third), 16, 128, 128);
  CheckYuv(Download(encoded_first), 16, 128, 128);
  CheckYuv(Download(encoded_second), 16, 128, 128);
  CheckYuv(Download(smaller), 16, 128, 128);
  CheckYuv(Download(ten_bit), 16, 128, 128);
  CheckYuv(Download(encoded_ten_bit), 16, 128, 128);
  CheckYuv(Download(uploaded_smaller), 16, 128, 128);
}

TEST_CASE(
    "CUDA converter shares pools across streams and cross-thread releases",
    "[.][frame-convter][cuda]") {
  constexpr std::size_t kThreads = 4;
  constexpr std::size_t kIterations = 6;
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 2 * 4096);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  struct Result {
    ffmpeg::Frame uploaded;
    cv::cuda::GpuMat image;
    ffmpeg::Frame output;
  };
  std::array<ffmpeg::Frame, kThreads> sources;
  std::array<std::vector<Result>, kThreads> results;
  std::array<std::exception_ptr, kThreads> errors{};
  std::array<std::thread, kThreads> workers;
  struct JoinWorkers {
    std::array<std::thread, kThreads>& threads;
    ~JoinWorkers() {
      for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
      }
    }
  } join_workers{workers};
  for (std::size_t index = 0; index < kThreads; ++index) {
    sources[index] =
        SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG,
                       16 + static_cast<int>(index) * 40, 128, 128, 64, 16);
    results[index].resize(kIterations);
  }
  const auto check_cuda = [](cudaError_t status) {
    if (status != cudaSuccess) {
      throw std::runtime_error(cudaGetErrorString(status));
    }
  };
  const auto check_driver = [](CUresult status) {
    if (status != CUDA_SUCCESS) {
      throw std::runtime_error("CUDA worker context operation failed");
    }
  };
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers[index] = std::thread([&, index] {
      cudaStream_t stream = nullptr;
      bool bound = false;
      try {
        check_driver(cuCtxPushCurrent(context));
        bound = true;
        check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        for (auto& result : results[index]) {
          result.uploaded = converter.CpuFrameToCuda(sources[index], stream);
          result.image =
              converter.ToMat(result.uploaded, RgbOrder::kBgr, stream);
          result.output = converter.ToFrame(result.image, result.uploaded,
                                            RgbOrder::kBgr, stream);
          check_cuda(cudaStreamSynchronize(stream));
        }
        check_cuda(cudaStreamDestroy(stream));
        stream = nullptr;
        CUcontext previous = nullptr;
        check_driver(cuCtxPopCurrent(&previous));
        bound = false;
      } catch (...) {
        errors[index] = std::current_exception();
        // Result storage is outside this try block, so queued work is drained
        // before any input/output owners can be destroyed on failure.
        if (stream) {
          cudaStreamSynchronize(stream);
          cudaStreamDestroy(stream);
        }
        if (bound) {
          CUcontext previous = nullptr;
          cuCtxPopCurrent(&previous);
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  for (const auto& error : errors) {
    if (error) std::rethrow_exception(error);
  }
  std::size_t pooled = 0;
  std::size_t fallback = 0;
  for (std::size_t index = 0; index < kThreads; ++index) {
    const int y = 16 + static_cast<int>(index) * 40;
    const auto gray = static_cast<std::uint8_t>(
        std::lround(static_cast<double>(y - 16) * 255 / 219));
    for (const auto& result : results[index]) {
      if (result.image.allocator == cv::cuda::GpuMat::getStdAllocator()) {
        ++fallback;
      } else {
        ++pooled;
      }
      CheckPixels(result.image, cv::Vec3b(gray, gray, gray), 1);
      CheckYuv(Download(result.output), y, 128, 128, 1);
    }
  }
  REQUIRE(pooled == 2);
  REQUIRE(fallback == kThreads * kIterations - pooled);
  for (std::size_t index = 0; index < kThreads; ++index) {
    workers[index] = std::thread([&, index] {
      bool bound = false;
      try {
        check_driver(cuCtxPushCurrent(context));
        bound = true;
        // Every image is freed by a different thread than its producer, with
        // reverse iteration order after all GPU operations have completed.
        auto& foreign_results = results[(index + 1) % kThreads];
        for (auto it = foreign_results.rbegin(); it != foreign_results.rend();
             ++it) {
          it->image.release();
          it->uploaded.Unref();
          it->output.Unref();
        }
        CUcontext previous = nullptr;
        check_driver(cuCtxPopCurrent(&previous));
        bound = false;
      } catch (...) {
        errors[index] = std::current_exception();
        if (bound) {
          CUcontext previous = nullptr;
          cuCtxPopCurrent(&previous);
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  for (const auto& error : errors) {
    if (error) std::rethrow_exception(error);
  }
  auto source = converter.CpuFrameToCuda(sources.front());
  auto first = converter.ToMat(source);
  auto second = converter.ToMat(source);
  REQUIRE(first.allocator != cv::cuda::GpuMat::getStdAllocator());
  REQUIRE(second.allocator != cv::cuda::GpuMat::getStdAllocator());
  REQUIRE(first.data != second.data);
  CheckPixels(first, cv::Vec3b(0, 0, 0));
  CheckPixels(second, cv::Vec3b(0, 0, 0));
}

TEST_CASE("CUDA converter rejects foreign frames even in their current context",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  CUcontext caller = nullptr;
  REQUIRE(cuCtxGetCurrent(&caller) == CUDA_SUCCESS);
  ffmpeg::HwDeviceContext separate(ffmpeg::HwDeviceType::kCuda, "0");
  OpencvCudaConvter foreign_converter(separate, 8192);
  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == caller);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto frame = foreign_converter.CpuFrameToCuda(software);
  const auto* hardware =
      reinterpret_cast<const AVHWDeviceContext*>(separate.get()->data);
  const auto* cuda = static_cast<const AVCUDADeviceContext*>(hardware->hwctx);
  TestContextBinding binding(cuda->cuda_ctx);
  auto image = foreign_converter.ToMat(frame);
  WaitForFrame(frame);
  REQUIRE_THROWS_AS(converter.ToMat(frame), std::invalid_argument);
  REQUIRE_THROWS_AS(converter.ToFrame(image, frame), std::invalid_argument);
}

TEST_CASE("CUDA converted frames retain their pool after converter destruction",
          "[.][frame-convter][cuda]") {
  auto frame = [] {
    auto device = PrimaryDevice();
    OpencvCudaConvter converter(device, 4096);
    auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                   AVCOL_RANGE_MPEG, 16, 128, 128, 64, 16);
    auto source = converter.CpuFrameToCuda(software);
    auto image = converter.ToMat(source);
    auto result = converter.ToFrame(image, source);
    WaitForFrame(result);
    return result;
  }();
  CheckYuv(Download(frame), 16, 128, 128);
}

TEST_CASE(
    "CUDA allocate uses metadata-only references and the output frame pool",
    "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  auto uploaded = converter.CpuFrameToCuda(software);
  WaitForFrame(uploaded);
  ffmpeg::Frame reference;
  reference.CopyPropertiesFrom(uploaded);
  reference->format = AV_PIX_FMT_CUDA;
  reference->width = uploaded->width;
  reference->height = uploaded->height;
  reference->hw_frames_ctx = av_buffer_ref(uploaded->hw_frames_ctx);
  REQUIRE(reference->hw_frames_ctx != nullptr);
  reference->crop_left = reference->crop_right = 2;
  reference->crop_top = reference->crop_bottom = 2;
  REQUIRE(reference->data[0] == nullptr);
  auto first = converter.allocate(reference, 64, 32);
  auto second = converter.allocate(reference, 64, 32);
  REQUIRE(first->format == AV_PIX_FMT_CUDA);
  REQUIRE(first->width == 64);
  REQUIRE(first->height == 32);
  REQUIRE(first->data[0] != nullptr);
  REQUIRE(first->data[1] != nullptr);
  const int minimum_stride = 64 * (format == AV_PIX_FMT_P010LE ? 2 : 1);
  REQUIRE(first->linesize[0] >= minimum_stride);
  REQUIRE(first->linesize[1] >= minimum_stride);
  REQUIRE(first->data[0] != second->data[0]);
  REQUIRE(first->hw_frames_ctx->data == second->hw_frames_ctx->data);
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(first->hw_frames_ctx->data);
  REQUIRE(pool->sw_format == format);
  REQUIRE(pool->device_ref->data == device.get()->data);
  REQUIRE(first->pts == reference->pts);
  REQUIRE(first->duration == reference->duration);
  REQUIRE(first->time_base.num == reference->time_base.num);
  REQUIRE(first->time_base.den == reference->time_base.den);
  REQUIRE(first->sample_aspect_ratio.num == 4);
  REQUIRE(first->sample_aspect_ratio.den == 3);
  REQUIRE(first->colorspace == reference->colorspace);
  REQUIRE(first->color_range == reference->color_range);
  REQUIRE(first->color_primaries == reference->color_primaries);
  REQUIRE(first->color_trc == reference->color_trc);
  REQUIRE(first->chroma_location == reference->chroma_location);
  REQUIRE(first->crop_left == 0);
  REQUIRE(first->crop_right == 0);
  REQUIRE(first->crop_top == 0);
  REQUIRE(first->crop_bottom == 0);
  const auto* entry = av_dict_get(first->metadata, "source", nullptr, 0);
  REQUIRE(entry != nullptr);
  REQUIRE(std::string(entry->value) == "cuda-test-camera");
  cv::cuda::GpuMat bgr(32, 64, CV_8UC3);
  bgr.setTo(cv::Scalar(0, 0, 0));
  auto converted = converter.ToFrame(bgr, reference);
  WaitForFrame(converted);
  REQUIRE(converted->hw_frames_ctx->data == first->hw_frames_ctx->data);
  auto* reusable = first->data[0];
  first.Unref();
  auto reused = converter.allocate(reference, 64, 32);
  REQUIRE(reused->data[0] == reusable);
  ZeroPixels(reused);
  CheckYuv(Download(reused), 0, 0, 0, 0);
}

TEST_CASE("CUDA allocate validates references and restores the calling context",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  OpencvCudaConvter converter(device, 0);
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto reference = converter.CpuFrameToCuda(software);
  WaitForFrame(reference);
  for (const auto dimensions : {std::pair<int, int>{0, 16},
                                {-2, 16},
                                {15, 16},
                                {16, 0},
                                {16, -2},
                                {16, 15}}) {
    REQUIRE_THROWS_AS(
        converter.allocate(reference, dimensions.first, dimensions.second),
        std::invalid_argument);
  }
  REQUIRE_THROWS_AS(converter.allocate(software, 32, 16),
                    std::invalid_argument);
  ffmpeg::HwDeviceContext separate(ffmpeg::HwDeviceType::kCuda, "0");
  OpencvCudaConvter foreign_converter(separate, 0);
  auto foreign = foreign_converter.CpuFrameToCuda(software);
  WaitForFrame(foreign);
  REQUIRE_THROWS_AS(converter.allocate(foreign, 32, 16), std::invalid_argument);
  const auto* hardware =
      reinterpret_cast<const AVHWDeviceContext*>(separate.get()->data);
  const auto* cuda = static_cast<const AVCUDADeviceContext*>(hardware->hwctx);
  TestContextBinding binding(cuda->cuda_ctx);
  auto allocated = converter.allocate(reference, 32, 16);
  REQUIRE(allocated->width == 32);
  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == cuda->cuda_ctx);
  CUcontext pixels = nullptr;
  REQUIRE(cuPointerGetAttribute(&pixels, CU_POINTER_ATTRIBUTE_CONTEXT,
                                reinterpret_cast<CUdeviceptr>(
                                    allocated->data[0])) == CUDA_SUCCESS);
  REQUIRE(pixels != restored);
  REQUIRE_THROWS_AS(converter.allocate(reference, 0, 16),
                    std::invalid_argument);
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == cuda->cuda_ctx);
}

TEST_CASE(
    "CUDA allocated frames can be filled after their converter is destroyed",
    "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  auto frame = [format] {
    auto device = PrimaryDevice();
    OpencvCudaConvter converter(device, 0);
    auto software =
        SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
    auto reference = converter.CpuFrameToCuda(software);
    WaitForFrame(reference);
    return converter.allocate(reference, 64, 32);
  }();
  ZeroPixels(frame);
  CheckYuv(Download(frame), 0, 0, 0, 0);
}

TEST_CASE("CUDA converted frames feed NVENC directly and flush encoded packets",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const bool shared_context = GENERATE(false, true);
  const char* encoder_name =
      format == AV_PIX_FMT_NV12 ? "h264_nvenc" : "hevc_nvenc";
  const auto* codec = avcodec_find_encoder_by_name(encoder_name);
  if (!codec) {
    SKIP(std::string("当前 FFmpeg 不提供 ") + encoder_name);
  }
  cv::cuda::setDevice(0);
  auto device = shared_context
                    ? PrimaryDevice()
                    : ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCuda, "0");
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  auto reference = converter.CpuFrameToCuda(software);
  cv::Mat host(360, 640, CV_8UC3, cv::Scalar(40, 120, 200));
  mw::streamer::AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    cv::cuda::GpuMat image;
    image.upload(host);
    auto output = converter.ToFrame(image, input);
    WaitForFrame(output);
    return output;
  });
  filter.Start();
  mw::streamer::AvDecoderReadResult input;
  input.status = mw::streamer::AvDecoderReadStatus::kFrame;
  input.frame = reference.Ref();
  auto converted = filter.ProcessVideo(std::move(input));
  filter.Stop();
  REQUIRE(converted.frame.has_value());
  auto frame = std::move(*converted.frame);
  frame->pts = 0;
  frame->duration = 1;
  frame->time_base = {1, 25};
  ffmpeg::CodecContext encoder(codec);
  auto* context = encoder.get();
  context->width = frame->width;
  context->height = frame->height;
  context->pix_fmt = AV_PIX_FMT_CUDA;
  context->sw_pix_fmt = format;
  context->time_base = {1, 25};
  context->framerate = {25, 1};
  context->bit_rate = 400000;
  context->gop_size = 12;
  context->max_b_frames = 0;
  context->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
  REQUIRE(context->hw_frames_ctx != nullptr);
  AVDictionary* options = nullptr;
  REQUIRE(av_dict_set(&options, "preset", "p1", 0) == 0);
  REQUIRE(av_dict_set(&options, "tune", "ull", 0) == 0);
  const int opened = avcodec_open2(context, codec, &options);
  av_dict_free(&options);
  INFO(encoder_name);
  INFO(ffmpeg::AvErrorStr(opened));
  if (opened == AVERROR_EXTERNAL || opened == AVERROR(ENOSYS)) {
    SKIP("当前环境没有可用的 NVENC 编码硬件或驱动");
  }
  REQUIRE(opened >= 0);
  REQUIRE(avcodec_send_frame(context, frame.get()) == 0);
  REQUIRE(avcodec_send_frame(context, nullptr) == 0);
  int packets = 0;
  ffmpeg::Packet packet;
  while (true) {
    const int received = avcodec_receive_packet(context, packet.get());
    if (received == AVERROR_EOF) {
      break;
    }
    INFO(ffmpeg::AvErrorStr(received));
    REQUIRE(received == 0);
    REQUIRE(packet->size > 0);
    ++packets;
    packet.Unref();
  }
  REQUIRE(packets > 0);
}

TEST_CASE(
    "CUDA filter callbacks convert in the decoder context without bridging",
    "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  cv::cuda::setDevice(0);
  CUcontext caller = nullptr;
  REQUIRE(cuCtxGetCurrent(&caller) == CUDA_SUCCESS);
  REQUIRE(caller != nullptr);
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda, "0");
  OpencvCudaConvter converter(device, 0);
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  auto source = converter.CpuFrameToCuda(software);
  CUcontext expected = nullptr;
  REQUIRE(cuPointerGetAttribute(
              &expected, CU_POINTER_ATTRIBUTE_CONTEXT,
              reinterpret_cast<CUdeviceptr>(source->data[0])) == CUDA_SUCCESS);
  REQUIRE(expected != caller);
  bool called = false;
  mw::streamer::AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    auto image = converter.ToMat(input);
    CUcontext current = nullptr;
    CUcontext pixels = nullptr;
    if (cuCtxGetCurrent(&current) != CUDA_SUCCESS ||
        cuPointerGetAttribute(&pixels, CU_POINTER_ATTRIBUTE_CONTEXT,
                              reinterpret_cast<CUdeviceptr>(image.data)) !=
            CUDA_SUCCESS ||
        current != expected || pixels != expected) {
      throw std::runtime_error("Filter conversion used another CUDA context");
    }
    // OpenCV operations between the two conversions must also use the same
    // context; the callback owns neither the context nor a staging copy.
    image.setTo(cv::Scalar(40, 120, 200));
    auto output = converter.ToFrame(image, input);
    WaitForFrame(output);
    if (output->hw_frames_ctx == nullptr) {
      throw std::runtime_error("Missing CUDA output pool");
    }
    called = true;
    return output;
  });
  filter.Start();
  mw::streamer::AvDecoderReadResult input;
  input.status = mw::streamer::AvDecoderReadStatus::kFrame;
  input.frame = source.Ref();
  const auto output = filter.ProcessVideo(std::move(input));
  filter.Stop();
  REQUIRE(called);
  REQUIRE(output.frame.has_value());
  CUcontext restored = nullptr;
  REQUIRE(cuCtxGetCurrent(&restored) == CUDA_SUCCESS);
  REQUIRE(restored == caller);
  const auto* input_pool =
      reinterpret_cast<const AVHWFramesContext*>(source->hw_frames_ctx->data);
  const auto* output_pool = reinterpret_cast<const AVHWFramesContext*>(
      (*output.frame)->hw_frames_ctx->data);
  REQUIRE(output_pool->device_ref->data == input_pool->device_ref->data);
  CheckYuv(Download(*output.frame), 129, 85, 166);
}

}  // namespace

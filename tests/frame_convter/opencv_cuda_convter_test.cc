#include "mw/frame_convter/opencv_cuda_convter.h"

#include <cuda.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <opencv2/core/cuda.hpp>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
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
                             AVColorRange range, int y, int u, int v) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->width = 34;
  frame->height = 18;
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
  ffmpeg::Frame result;
  REQUIRE(av_hwframe_transfer_data(result.get(), source.get(), 0) >= 0);
  return result;
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
  SECTION("black and white honor native range") {
    const bool white = GENERATE(false, true);
    const int y = limited ? (white ? 235 : 16) : (white ? 255 : 0);
    auto software = SoftwareSample(format, matrix, range, y, 128, 128);
    auto frame = OpencvCudaConvter::CpuFrameToCuda(software, device);
    auto bgr = OpencvCudaConvter::ToMat(frame);
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
    auto frame = OpencvCudaConvter::CpuFrameToCuda(software, device);
    auto bgr = OpencvCudaConvter::ToMat(frame);
    auto rgb = OpencvCudaConvter::ToMat(frame, RgbOrder::kRgb);
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
  auto software = SoftwareSample(format, matrix, range, 128, 128, 128);
  auto reference = OpencvCudaConvter::CpuFrameToCuda(software, device);
  reference->crop_left = 2;
  reference->crop_right = 2;
  cv::Mat input(
      16, 28, CV_8UC3,
      order == RgbOrder::kBgr ? cv::Scalar(0, 0, 255) : cv::Scalar(255, 0, 0));
  cv::cuda::GpuMat storage;
  storage.upload(input);
  auto image = storage(cv::Rect(3, 2, 20, 12));
  REQUIRE_FALSE(image.isContinuous());
  auto frame = OpencvCudaConvter::ToFrame(image, reference, order);
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
  storage.setTo(cv::Scalar(0, 0, 0));
  reference.Unref();
  const bool limited = range == AVCOL_RANGE_MPEG;
  const bool bt709 = matrix != AVCOL_SPC_SMPTE170M;
  const int y = limited ? (bt709 ? 63 : 81) : (bt709 ? 54 : 76);
  const int u = limited ? (bt709 ? 102 : 90) : (bt709 ? 99 : 85);
  CheckYuv(Download(frame), y, u, limited ? 240 : 255);
  CheckPixels(OpencvCudaConvter::ToMat(frame), cv::Vec3b(0, 0, 255), 5);
}

TEST_CASE("CUDA reverse conversion defaults to OpenCV BGR order",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto reference = OpencvCudaConvter::CpuFrameToCuda(software, device);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  auto frame = OpencvCudaConvter::ToFrame(image, reference);
  CheckYuv(Download(frame), 63, 102, 240);
}

TEST_CASE(
    "CUDA conversion rejects unsupported frames images and color metadata",
    "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 16, 128, 128);
  auto frame = OpencvCudaConvter::CpuFrameToCuda(software, device);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 0));
  cv::cuda::GpuMat image;
  image.upload(host);
  SECTION("CPU frame") {
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(software),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, software),
                      std::invalid_argument);
  }
  SECTION("missing hardware pool") {
    av_buffer_unref(&frame->hw_frames_ctx);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
  }
  SECTION("unsupported matrix") {
    frame->colorspace = AVCOL_SPC_BT2020_CL;
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
  }
  SECTION("missing chroma location") {
    frame->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
  }
  SECTION("missing range") {
    frame->color_range = AVCOL_RANGE_UNSPECIFIED;
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
  }
  SECTION("HDR requires explicit tone mapping") {
    frame->color_trc = GENERATE(AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
  }
  SECTION("empty or non-three-channel byte image") {
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(cv::cuda::GpuMat{}, frame),
                      std::invalid_argument);
    cv::cuda::GpuMat wrong_type(12, 20, CV_8UC4);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(wrong_type, frame),
                      std::invalid_argument);
  }
  SECTION("odd dimensions") {
    auto odd = image(cv::Rect(0, 0, 19, 12));
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(odd, frame),
                      std::invalid_argument);
    --frame->width;
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
  }
  SECTION("invalid RGB order") {
    const auto order = static_cast<RgbOrder>(999);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame, order),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame, order),
                      std::invalid_argument);
  }
  SECTION("unsupported hardware software format") {
    auto* pool =
        reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    pool->sw_format = AV_PIX_FMT_YUV444P;
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
    REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                      std::invalid_argument);
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
  auto frame = OpencvCudaConvter::CpuFrameToCuda(software, separate);
  cv::Mat host(18, 34, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  REQUIRE_THROWS_AS(OpencvCudaConvter::ToMat(frame), std::invalid_argument);
  REQUIRE_THROWS_AS(OpencvCudaConvter::ToFrame(image, frame),
                    std::invalid_argument);
  CUcontext after = nullptr;
  REQUIRE(cuCtxGetCurrent(&after) == CUDA_SUCCESS);
  REQUIRE(after == before);
}

TEST_CASE("CUDA conversion samples chroma according to native chroma location",
          "[.][frame-convter][cuda]") {
  const auto format = GENERATE(AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE);
  const bool vertical = GENERATE(false, true);
  auto device = PrimaryDevice();
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
  auto frame = OpencvCudaConvter::CpuFrameToCuda(software, device);
  frame->chroma_location = vertical ? AVCHROMA_LOC_TOP : AVCHROMA_LOC_LEFT;
  cv::Mat first;
  OpencvCudaConvter::ToMat(frame).download(first);
  frame->chroma_location = vertical ? AVCHROMA_LOC_BOTTOM : AVCHROMA_LOC_CENTER;
  cv::Mat second;
  OpencvCudaConvter::ToMat(frame).download(second);
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
  auto software =
      SoftwareSample(AV_PIX_FMT_NV12, matrix, AVCOL_RANGE_JPEG, 128, 128, 128);
  software->chroma_location = AVCHROMA_LOC_CENTER;
  auto reference = OpencvCudaConvter::CpuFrameToCuda(software, device);
  cv::cuda::GpuMat image;
  image.upload(host);
  auto output = OpencvCudaConvter::ToFrame(image, reference, order);
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
  auto software = SoftwareSample(AV_PIX_FMT_NV12, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 63, 102, 240);
  software->chroma_location = location;
  auto frame = OpencvCudaConvter::CpuFrameToCuda(software, device);
  auto image = OpencvCudaConvter::ToMat(frame);
  CheckPixels(image, cv::Vec3b(0, 0, 255));
  auto output = OpencvCudaConvter::ToFrame(image, frame);
  REQUIRE(output->chroma_location == location);
  CheckYuv(Download(output), 63, 102, 240);
}

TEST_CASE("CUDA P010 conversion computes ten-bit samples before packing",
          "[.][frame-convter][cuda]") {
  auto device = PrimaryDevice();
  auto software = SoftwareSample(AV_PIX_FMT_P010LE, AVCOL_SPC_BT709,
                                 AVCOL_RANGE_MPEG, 64, 128, 128);
  auto reference = OpencvCudaConvter::CpuFrameToCuda(software, device);
  cv::Mat host(12, 20, CV_8UC3, cv::Scalar(0, 0, 255));
  cv::cuda::GpuMat image;
  image.upload(host);
  auto output = OpencvCudaConvter::ToFrame(image, reference);
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
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  auto reference = OpencvCudaConvter::CpuFrameToCuda(software, device);
  cv::Mat host(360, 640, CV_8UC3, cv::Scalar(40, 120, 200));
  mw::streamer::AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    cv::cuda::GpuMat image;
    image.upload(host);
    return OpencvCudaConvter::ToFrame(image, input);
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
  auto software =
      SoftwareSample(format, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 16, 128, 128);
  auto source = OpencvCudaConvter::CpuFrameToCuda(software, device);
  CUcontext expected = nullptr;
  REQUIRE(cuPointerGetAttribute(
              &expected, CU_POINTER_ATTRIBUTE_CONTEXT,
              reinterpret_cast<CUdeviceptr>(source->data[0])) == CUDA_SUCCESS);
  REQUIRE(expected != caller);
  bool called = false;
  mw::streamer::AvMediaFilter filter;
  filter.SetOnVideo([&](const ffmpeg::Frame& input) {
    auto image = OpencvCudaConvter::ToMat(input);
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
    auto output = OpencvCudaConvter::ToFrame(image, input);
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

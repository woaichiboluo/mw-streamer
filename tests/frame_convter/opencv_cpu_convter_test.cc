#include "mw/frame_convter/opencv_cpu_convter.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/dict.h>
}

namespace {

using mw::frame_convter::OpencvCpuConvter;
using mw::frame_convter::RgbOrder;
namespace ffmpeg = mw::streamer::ffmpeg;

ffmpeg::Frame Reference(AVPixelFormat format) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->width = 18;
  frame->height = 10;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_range = AVCOL_RANGE_MPEG;
  if (format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24) {
    frame->colorspace = AVCOL_SPC_RGB;
    frame->color_range = AVCOL_RANGE_JPEG;
  }
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  frame->pts = 12345;
  frame->time_base = {1, 90000};
  frame->duration = 3000;
  frame->sample_aspect_ratio = {4, 3};
  REQUIRE(av_dict_set(&frame->metadata, "source", "test-camera", 0) == 0);
  return frame;
}

ffmpeg::Frame Gray(AVColorRange range, std::uint8_t luma) {
  auto frame = Reference(AV_PIX_FMT_YUV420P);
  frame->color_range = range;
  REQUIRE(av_frame_get_buffer(frame.get(), 64) == 0);
  for (int plane = 0; plane < 3; ++plane) {
    const int width = plane == 0 ? frame->width : frame->width / 2;
    const int height = plane == 0 ? frame->height : frame->height / 2;
    for (int row = 0; row < height; ++row) {
      std::fill_n(frame->data[plane] + row * frame->linesize[plane], width,
                  plane == 0 ? luma : 128);
    }
  }
  return frame;
}

void CheckNear(const cv::Mat& image, const cv::Vec3b& expected,
               int tolerance = 3) {
  REQUIRE(image.type() == CV_8UC3);
  for (int row = 0; row < image.rows; ++row) {
    for (int column = 0; column < image.cols; ++column) {
      const auto actual = image.at<cv::Vec3b>(row, column);
      for (int channel = 0; channel < 3; ++channel) {
        CAPTURE(row, column, channel);
        REQUIRE(std::abs(static_cast<int>(actual[channel]) -
                         expected[channel]) <= tolerance);
      }
    }
  }
}

TEST_CASE(
    "CPU frame conversion preserves channel order and independent storage",
    "[frame-convter][cpu]") {
  auto reference = Reference(AV_PIX_FMT_RGB24);
  cv::Mat storage(14, 30, CV_8UC3, cv::Scalar(17, 80, 220));
  const auto image = storage(cv::Rect(3, 2, 18, 10));
  REQUIRE_FALSE(image.isContinuous());
  auto frame = OpencvCpuConvter::ToFrame(image, reference);
  REQUIRE(frame->format == AV_PIX_FMT_RGB24);
  REQUIRE(frame->data[0][0] == 220);
  REQUIRE(frame->data[0][1] == 80);
  REQUIRE(frame->data[0][2] == 17);
  storage.setTo(cv::Scalar(0, 0, 0));
  auto bgr = OpencvCpuConvter::ToMat(frame);
  CheckNear(bgr, cv::Vec3b(17, 80, 220), 0);
  auto rgb = OpencvCpuConvter::ToMat(frame, RgbOrder::kRgb);
  CheckNear(rgb, cv::Vec3b(220, 80, 17), 0);
  auto rgb_frame = OpencvCpuConvter::ToFrame(rgb, reference, RgbOrder::kRgb);
  CheckNear(OpencvCpuConvter::ToMat(rgb_frame), cv::Vec3b(17, 80, 220), 0);
  frame.Unref();
  CheckNear(bgr, cv::Vec3b(17, 80, 220), 0);
}

TEST_CASE("CPU frame conversion uses native range and matrix metadata",
          "[frame-convter][cpu]") {
  SECTION("limited and full luma are interpreted differently") {
    auto limited = Gray(AVCOL_RANGE_MPEG, 16);
    auto full = Gray(AVCOL_RANGE_JPEG, 16);
    CheckNear(OpencvCpuConvter::ToMat(limited, RgbOrder::kBgr),
              cv::Vec3b(0, 0, 0), 1);
    CheckNear(OpencvCpuConvter::ToMat(full, RgbOrder::kBgr),
              cv::Vec3b(16, 16, 16), 1);
  }
  SECTION("BT601 and BT709 red samples") {
    const bool bt709 = GENERATE(false, true);
    auto frame = Gray(AVCOL_RANGE_MPEG, bt709 ? 63 : 81);
    frame->colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    for (int row = 0; row < frame->height / 2; ++row) {
      std::fill_n(frame->data[1] + row * frame->linesize[1], frame->width / 2,
                  bt709 ? 102 : 90);
      std::fill_n(frame->data[2] + row * frame->linesize[2], frame->width / 2,
                  240);
    }
    CheckNear(OpencvCpuConvter::ToMat(frame, RgbOrder::kBgr),
              cv::Vec3b(0, 0, 255), 3);
  }
}

TEST_CASE("CPU frame conversion accepts bottom-up software frames",
          "[frame-convter][cpu]") {
  auto reference = Reference(AV_PIX_FMT_BGR24);
  cv::Mat image(10, 18, CV_8UC3);
  for (int row = 0; row < image.rows; ++row) {
    image.row(row).setTo(cv::Scalar(row * 10, 30, 200));
  }
  auto frame = OpencvCpuConvter::ToFrame(image, reference);
  frame->data[0] += (frame->height - 1) * frame->linesize[0];
  frame->linesize[0] = -frame->linesize[0];
  const auto flipped = OpencvCpuConvter::ToMat(frame, RgbOrder::kBgr);
  for (int row = 0; row < image.rows; ++row) {
    CheckNear(flipped.row(row),
              cv::Vec3b(static_cast<std::uint8_t>((image.rows - row - 1) * 10),
                        30, 200),
              0);
  }
}

TEST_CASE("CPU frame conversion supports planar packed and high-depth YUV",
          "[frame-convter][cpu]") {
  const auto format =
      GENERATE(AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
               AV_PIX_FMT_YUYV422, AV_PIX_FMT_UYVY422, AV_PIX_FMT_NV12,
               AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE);
  CAPTURE(format);
  auto reference = Reference(format);
  cv::Mat input(12, 20, CV_8UC3, cv::Scalar(40, 120, 200));
  auto frame = OpencvCpuConvter::ToFrame(input, reference);
  REQUIRE(frame->format == format);
  REQUIRE(frame->width == input.cols);
  REQUIRE(frame->height == input.rows);
  CheckNear(OpencvCpuConvter::ToMat(frame, RgbOrder::kBgr),
            cv::Vec3b(40, 120, 200), 4);
  REQUIRE(frame->pts == reference->pts);
  REQUIRE(frame->duration == reference->duration);
  REQUIRE(frame->time_base.num == 1);
  REQUIRE(frame->time_base.den == 90000);
  REQUIRE(frame->sample_aspect_ratio.num == 4);
  REQUIRE(frame->sample_aspect_ratio.den == 3);
  REQUIRE(frame->colorspace == reference->colorspace);
  REQUIRE(frame->color_range == reference->color_range);
  REQUIRE(frame->color_primaries == reference->color_primaries);
  REQUIRE(frame->color_trc == reference->color_trc);
  REQUIRE(frame->chroma_location == reference->chroma_location);
  const auto* entry = av_dict_get(frame->metadata, "source", nullptr, 0);
  REQUIRE(entry != nullptr);
  REQUIRE(std::string(entry->value) == "test-camera");
}

TEST_CASE("CPU frame Convert uses reference format and source dimensions",
          "[frame-convter][cpu]") {
  auto source = Gray(AVCOL_RANGE_MPEG, 235);
  auto reference = Reference(AV_PIX_FMT_NV12);
  reference->width = 100;
  reference->height = 100;
  reference->pts = 999;
  auto converted = OpencvCpuConvter::Convert(source, reference);
  REQUIRE(converted->format == AV_PIX_FMT_NV12);
  REQUIRE(converted->width == source->width);
  REQUIRE(converted->height == source->height);
  REQUIRE(converted->pts == 999);
  for (int plane = 0; plane < 2; ++plane) {
    const int height = plane == 0 ? converted->height : converted->height / 2;
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < converted->width; ++column) {
        REQUIRE(
            converted->data[plane][row * converted->linesize[plane] + column] ==
            (plane == 0 ? 235 : 128));
      }
    }
  }
  // libswscale's RGB output rounding can differ at the final rows even for
  // uniform white; the YUV layout conversion above must remain exact.
  CheckNear(OpencvCpuConvter::ToMat(converted, RgbOrder::kBgr),
            cv::Vec3b(255, 255, 255), 3);
}

TEST_CASE(
    "CPU frame Convert transforms YUV matrix instead of relabeling pixels",
    "[frame-convter][cpu]") {
  auto source = Gray(AVCOL_RANGE_MPEG, 81);
  source->colorspace = AVCOL_SPC_SMPTE170M;
  for (int row = 0; row < source->height / 2; ++row) {
    std::fill_n(source->data[1] + row * source->linesize[1], source->width / 2,
                90);
    std::fill_n(source->data[2] + row * source->linesize[2], source->width / 2,
                240);
  }
  auto reference = Reference(AV_PIX_FMT_YUV420P);
  auto converted = OpencvCpuConvter::Convert(source, reference);
  REQUIRE(converted->colorspace == AVCOL_SPC_BT709);
  REQUIRE(std::abs(static_cast<int>(converted->data[0][0]) - 63) <= 3);
  REQUIRE(std::abs(static_cast<int>(converted->data[1][0]) - 102) <= 3);
  REQUIRE(std::abs(static_cast<int>(converted->data[2][0]) - 240) <= 3);
  // Matrix conversion quantizes YUV before ToMat performs a second conversion.
  // Verify native YUV values above and allow the accumulated RGB rounding here.
  CheckNear(OpencvCpuConvter::ToMat(converted, RgbOrder::kBgr),
            cv::Vec3b(0, 0, 255), 6);
}

TEST_CASE("CPU frame Convert resamples a changed chroma location",
          "[frame-convter][cpu]") {
  const auto format = GENERATE(AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE);
  const bool high_depth = format == AV_PIX_FMT_YUV420P10LE;
  auto source = Reference(format);
  REQUIRE(av_frame_get_buffer(source.get(), 64) == 0);
  for (int plane = 0; plane < 3; ++plane) {
    const int width = plane == 0 ? source->width : source->width / 2;
    const int height = plane == 0 ? source->height : source->height / 2;
    for (int row = 0; row < height; ++row) {
      auto* pixels = source->data[plane] + row * source->linesize[plane];
      for (int column = 0; column < width; ++column) {
        if (high_depth) {
          reinterpret_cast<std::uint16_t*>(pixels)[column] =
              static_cast<std::uint16_t>(
                  plane == 1 ? (column < width / 2 ? 129 : 897) : 512);
        } else {
          pixels[column] = static_cast<std::uint8_t>(
              plane == 1 ? (column < width / 2 ? 32 : 224) : 128);
        }
      }
    }
  }
  auto reference = Reference(format);
  reference->chroma_location = AVCHROMA_LOC_CENTER;
  auto converted = OpencvCpuConvter::Convert(source, reference);
  REQUIRE(converted->chroma_location == AVCHROMA_LOC_CENTER);
  // Left-to-center shifts chroma one quarter of a chroma sample to the right.
  // At the step edge this produces 32+(224-32)/4, or its ten-bit counterpart.
  const int edge = source->width / 4 - 1;
  for (int row = 0; row < source->height / 2; ++row) {
    CAPTURE(format, row);
    const auto* chroma = converted->data[1] + row * converted->linesize[1];
    const int value = high_depth
                          ? reinterpret_cast<const std::uint16_t*>(chroma)[edge]
                          : chroma[edge];
    REQUIRE(value == (high_depth ? 321 : 80));
  }
}

TEST_CASE(
    "CPU frame Convert retains ten-bit samples without an RGB8 intermediate",
    "[frame-convter][cpu]") {
  auto source = Reference(AV_PIX_FMT_YUV420P10LE);
  REQUIRE(av_frame_get_buffer(source.get(), 64) == 0);
  for (int plane = 0; plane < 3; ++plane) {
    const int width = plane == 0 ? source->width : source->width / 2;
    const int height = plane == 0 ? source->height : source->height / 2;
    for (int row = 0; row < height; ++row) {
      auto* pixels = reinterpret_cast<std::uint16_t*>(
          source->data[plane] + row * source->linesize[plane]);
      for (int column = 0; column < width; ++column) {
        pixels[column] = static_cast<std::uint16_t>(
            64 + (row * 37 + column * 11 + plane * 17) % 800);
      }
    }
  }
  auto reference = Reference(AV_PIX_FMT_P010LE);
  auto p010 = OpencvCpuConvter::Convert(source, reference);
  auto restored = OpencvCpuConvter::Convert(p010, source);
  for (int plane = 0; plane < 3; ++plane) {
    const int width = plane == 0 ? source->width : source->width / 2;
    const int height = plane == 0 ? source->height : source->height / 2;
    for (int row = 0; row < height; ++row) {
      const auto* expected = reinterpret_cast<const std::uint16_t*>(
          source->data[plane] + row * source->linesize[plane]);
      const auto* actual = reinterpret_cast<const std::uint16_t*>(
          restored->data[plane] + row * restored->linesize[plane]);
      for (int column = 0; column < width; ++column) {
        CAPTURE(plane, row, column);
        REQUIRE(actual[column] == expected[column]);
      }
    }
  }
}

TEST_CASE("CPU frame conversion rejects invalid inputs and missing color data",
          "[frame-convter][cpu]") {
  auto valid = Gray(AVCOL_RANGE_MPEG, 128);
  auto reference = Reference(AV_PIX_FMT_YUV420P);
  cv::Mat image(10, 18, CV_8UC3, cv::Scalar(0, 0, 0));
  SECTION("empty or wrong-type Mat") {
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToFrame({}, reference),
                      std::invalid_argument);
    cv::Mat gray(10, 18, CV_8UC1);
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToFrame(gray, reference),
                      std::invalid_argument);
  }
  SECTION("missing color matrix") {
    valid->colorspace = AVCOL_SPC_UNSPECIFIED;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("missing color range") {
    reference->color_range = AVCOL_RANGE_UNSPECIFIED;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToFrame(image, reference),
                      std::invalid_argument);
  }
  SECTION("hardware format") {
    reference->format = AV_PIX_FMT_CUDA;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToFrame(image, reference),
                      std::invalid_argument);
    valid->format = AV_PIX_FMT_CUDA;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("missing source data") {
    auto empty = Reference(AV_PIX_FMT_YUV420P);
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(empty, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("invalid source stride") {
    valid->linesize[0] = 1;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("moved-from frame") {
    auto owner = std::move(valid);
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("HDR conversion requires an explicit tone mapper") {
    valid->color_trc = AVCOL_TRC_SMPTE2084;
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, RgbOrder::kBgr),
                      std::invalid_argument);
  }
  SECTION("Convert cannot silently change transfer functions") {
    reference->color_trc = AVCOL_TRC_GAMMA22;
    REQUIRE_THROWS_AS(OpencvCpuConvter::Convert(valid, reference),
                      std::invalid_argument);
  }
  SECTION("Convert cannot silently change color primaries") {
    reference->color_primaries = AVCOL_PRI_BT2020;
    REQUIRE_THROWS_AS(OpencvCpuConvter::Convert(valid, reference),
                      std::invalid_argument);
  }
  SECTION("invalid RGB order") {
    const auto invalid_order = static_cast<RgbOrder>(999);
    REQUIRE_THROWS_AS(OpencvCpuConvter::ToMat(valid, invalid_order),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(
        OpencvCpuConvter::ToFrame(image, reference, invalid_order),
        std::invalid_argument);
  }
}

}  // namespace

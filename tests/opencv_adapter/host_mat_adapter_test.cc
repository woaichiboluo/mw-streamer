#include "mw/opencv_adapter/host_mat_adapter.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <opencv2/core/mat.hpp>
#include <vector>

#include "mw/opencv_adapter/cuda_frame.h"

namespace {

using mw::streamer::opencv_adapter::CudaFrame;
using mw::streamer::opencv_adapter::HostMatAdapter;

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

MwStreamerMediaTimestamp MakeTimestamp() { return {1234, 40, {1, 1000}}; }

void CheckBlackAndWhite(const MwStreamerVideoFrameView& source,
                        int expected_mat_type) {
  const auto check_mat = [&](const cv::Mat& bgr) {
    REQUIRE_FALSE(bgr.empty());
    REQUIRE(bgr.type() == expected_mat_type);
    if (expected_mat_type == CV_8UC3) {
      const auto black = bgr.at<cv::Vec3b>(0, 0);
      const auto white = bgr.at<cv::Vec3b>(bgr.rows - 1, bgr.cols - 1);
      for (int channel = 0; channel < 3; ++channel) {
        CHECK(black[channel] <= 1);
        CHECK(white[channel] >= 250);
      }
      return;
    }

    const auto black = bgr.at<cv::Vec<std::uint16_t, 3>>(0, 0);
    const auto white =
        bgr.at<cv::Vec<std::uint16_t, 3>>(bgr.rows - 1, bgr.cols - 1);
    for (int channel = 0; channel < 3; ++channel) {
      CHECK(black[channel] <= 64);
      CHECK(white[channel] >= 65000);
    }
  };

  const cv::Mat host_bgr = HostMatAdapter::ToBgr(source);
  check_mat(host_bgr);

  const auto cuda_source = CudaFrame::CopyFrom(source);
  const cv::Mat cuda_bgr = HostMatAdapter::ToBgr(cuda_source.view());
  check_mat(cuda_bgr);

  const auto converted = HostMatAdapter::FromBgr(cuda_bgr, cuda_source.view());
  CHECK(converted.view().buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(converted.view().buffer.pixel_format == source.buffer.pixel_format);
  CHECK(converted.view().buffer.storage.linear.plane_count ==
        source.buffer.storage.linear.plane_count);
}

TEST_CASE("HostMatAdapter在NV12和BGR8之间同步转换") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint8_t> y(kWidth * kHeight);
  std::vector<std::uint8_t> uv(kWidth * kHeight / 2, 128);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    const std::uint8_t value = row < kHeight / 2 ? 16 : 235;
    std::fill_n(y.data() + row * kWidth, kWidth, value);
  }
  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
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
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };

  cv::Mat bgr = HostMatAdapter::ToBgr(source);
  REQUIRE_FALSE(bgr.empty());
  CHECK(bgr.type() == CV_8UC3);
  CHECK(bgr.cols == static_cast<int>(kWidth));
  CHECK(bgr.rows == static_cast<int>(kHeight));
  const auto black = bgr.at<cv::Vec3b>(0, 0);
  const auto white = bgr.at<cv::Vec3b>(kHeight - 1, kWidth - 1);
  for (int channel = 0; channel < 3; ++channel) {
    CHECK(black[channel] <= 1);
    CHECK(white[channel] >= 250);
  }

  auto converted = HostMatAdapter::FromBgr(bgr, source);
  const auto& converted_view = converted.view();
  CHECK(converted_view.buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(converted_view.buffer.pixel_format == kMwStreamerVideoPixelFormatNv12);
  CHECK(converted_view.color.space == kMwStreamerColorSpaceBt709);
  CHECK(converted_view.timestamp.pts == 1234);

  cv::Mat round_trip = HostMatAdapter::ToBgr(converted_view);
  REQUIRE(round_trip.type() == CV_8UC3);
  for (int row = 0; row < bgr.rows; ++row) {
    for (int column = 0; column < bgr.cols; ++column) {
      const auto expected = bgr.at<cv::Vec3b>(row, column);
      const auto actual = round_trip.at<cv::Vec3b>(row, column);
      for (int channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(static_cast<int>(actual[channel]) -
                       static_cast<int>(expected[channel])) <= 2);
      }
    }
  }

  cv::Mat blue(kHeight, kWidth, CV_8UC3, cv::Scalar(255, 0, 0));
  auto blue_frame = HostMatAdapter::FromBgr(blue, source);
  cv::Mat blue_round_trip = HostMatAdapter::ToBgr(blue_frame.view());
  const auto blue_pixel = blue_round_trip.at<cv::Vec3b>(0, 0);
  CHECK(blue_pixel[0] >= 250);
  CHECK(blue_pixel[1] <= 2);
  CHECK(blue_pixel[2] <= 2);
}

TEST_CASE("HostMatAdapter在P010和BGR16之间同步转换") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint16_t> y(kWidth * kHeight);
  std::vector<std::uint16_t> uv(kWidth * kHeight / 2, 512U << 6);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    const std::uint16_t value = row < kHeight / 2 ? 64U << 6 : 940U << 6;
    std::fill_n(y.data() + row * kWidth, kWidth, value);
  }
  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data()), kWidth * 2, kWidth * 2,
       kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kWidth * 2, kWidth * 2,
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

  cv::Mat bgr = HostMatAdapter::ToBgr(source);
  REQUIRE_FALSE(bgr.empty());
  CHECK(bgr.type() == CV_16UC3);
  const auto black = bgr.at<cv::Vec<std::uint16_t, 3>>(0, 0);
  const auto white = bgr.at<cv::Vec<std::uint16_t, 3>>(kHeight - 1, kWidth - 1);
  for (int channel = 0; channel < 3; ++channel) {
    CHECK(black[channel] <= 64);
    CHECK(white[channel] >= 65000);
  }

  auto converted = HostMatAdapter::FromBgr(bgr, source);
  const auto& converted_view = converted.view();
  CHECK(converted_view.buffer.pixel_format == kMwStreamerVideoPixelFormatP010);
  REQUIRE(converted_view.buffer.storage.linear.plane_count == 2);
  for (std::uint32_t plane_index = 0; plane_index < 2; ++plane_index) {
    const auto& plane =
        converted_view.buffer.storage.linear.planes[plane_index];
    const auto* values = reinterpret_cast<const std::uint16_t*>(plane.address);
    const std::size_t value_count =
        static_cast<std::size_t>(plane.row_bytes / 2) * plane.row_count;
    for (std::size_t index = 0; index < value_count; ++index) {
      CHECK((values[index] & 0x3fU) == 0);
    }
  }

  cv::Mat round_trip = HostMatAdapter::ToBgr(converted_view);
  CHECK(round_trip.type() == CV_16UC3);
}

TEST_CASE("HostMatAdapter转换P016和YUV444P16") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;

  SECTION("P016") {
    std::vector<std::uint16_t> y(kWidth * kHeight);
    std::vector<std::uint16_t> uv(kWidth * kHeight / 2, 32768);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
      const std::uint16_t value = row < kHeight / 2 ? 4096 : 60160;
      std::fill_n(y.data() + row * kWidth, kWidth, value);
    }
    const std::array<MwStreamerVideoPlaneView, 2> planes = {{
        {reinterpret_cast<std::uintptr_t>(y.data()), kWidth * 2, kWidth * 2,
         kHeight},
        {reinterpret_cast<std::uintptr_t>(uv.data()), kWidth * 2, kWidth * 2,
         kHeight / 2},
    }};
    const MwStreamerVideoFrameView source = {
        {kMwStreamerMemoryHost,
         kMwStreamerVideoStorageLinear,
         kMwStreamerVideoPixelFormatP016,
         kWidth,
         kHeight,
         {.linear = {planes.data(),
                     static_cast<std::uint32_t>(planes.size())}}},
        MakeColorInfo(),
        MakeTimestamp(),
    };
    CheckBlackAndWhite(source, CV_16UC3);
  }

  SECTION("YUV444P16") {
    std::vector<std::uint16_t> y(kWidth * kHeight);
    std::vector<std::uint16_t> u(kWidth * kHeight, 32768);
    std::vector<std::uint16_t> v(kWidth * kHeight, 32768);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
      const std::uint16_t value = row < kHeight / 2 ? 4096 : 60160;
      std::fill_n(y.data() + row * kWidth, kWidth, value);
    }
    const std::array<MwStreamerVideoPlaneView, 3> planes = {{
        {reinterpret_cast<std::uintptr_t>(y.data()), kWidth * 2, kWidth * 2,
         kHeight},
        {reinterpret_cast<std::uintptr_t>(u.data()), kWidth * 2, kWidth * 2,
         kHeight},
        {reinterpret_cast<std::uintptr_t>(v.data()), kWidth * 2, kWidth * 2,
         kHeight},
    }};
    const MwStreamerVideoFrameView source = {
        {kMwStreamerMemoryHost,
         kMwStreamerVideoStorageLinear,
         kMwStreamerVideoPixelFormatYuv444p16le,
         kWidth,
         kHeight,
         {.linear = {planes.data(),
                     static_cast<std::uint32_t>(planes.size())}}},
        MakeColorInfo(),
        MakeTimestamp(),
    };
    CheckBlackAndWhite(source, CV_16UC3);
  }
}

TEST_CASE("HostMatAdapter转换YUV444P") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint8_t> y(kWidth * kHeight);
  std::vector<std::uint8_t> u(kWidth * kHeight, 128);
  std::vector<std::uint8_t> v(kWidth * kHeight, 128);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    const std::uint8_t value = row < kHeight / 2 ? 16 : 235;
    std::fill_n(y.data() + row * kWidth, kWidth, value);
  }
  const std::array<MwStreamerVideoPlaneView, 3> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data()), kWidth, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(u.data()), kWidth, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(v.data()), kWidth, kWidth, kHeight},
  }};
  const MwStreamerVideoFrameView source = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatYuv444p,
       kWidth,
       kHeight,
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };
  CheckBlackAndWhite(source, CV_8UC3);
}

TEST_CASE("HostMatAdapter接受CUDA View和prototype") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint8_t> y(kWidth * kHeight);
  std::vector<std::uint8_t> uv(kWidth * kHeight / 2, 128);
  for (std::uint32_t row = 0; row < kHeight; ++row) {
    const std::uint8_t value = row < kHeight / 2 ? 16 : 235;
    std::fill_n(y.data() + row * kWidth, kWidth, value);
  }
  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
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
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };
  const auto cuda_source = CudaFrame::CopyFrom(source);

  const cv::Mat bgr = HostMatAdapter::ToBgr(cuda_source.view());
  REQUIRE(bgr.type() == CV_8UC3);
  const auto black = bgr.at<cv::Vec3b>(0, 0);
  const auto white = bgr.at<cv::Vec3b>(kHeight - 1, kWidth - 1);
  for (int channel = 0; channel < 3; ++channel) {
    CHECK(black[channel] <= 1);
    CHECK(white[channel] >= 250);
  }

  const auto converted = HostMatAdapter::FromBgr(bgr, cuda_source.view());
  CHECK(converted.view().buffer.memory_type == kMwStreamerMemoryHost);
  CHECK(converted.view().buffer.pixel_format ==
        kMwStreamerVideoPixelFormatNv12);
  CHECK(converted.view().color.space == kMwStreamerColorSpaceBt709);
  CHECK(converted.view().timestamp.pts == 1234);
}

TEST_CASE("HostMatAdapter拒绝HDR和不匹配的Mat") {
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::vector<std::uint8_t> y(kWidth * kHeight, 16);
  std::vector<std::uint8_t> uv(kWidth * kHeight / 2, 128);
  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data()), kWidth, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kWidth, kWidth,
       kHeight / 2},
  }};
  MwStreamerVideoFrameView prototype = {
      {kMwStreamerMemoryHost,
       kMwStreamerVideoStorageLinear,
       kMwStreamerVideoPixelFormatNv12,
       kWidth,
       kHeight,
       {.linear = {planes.data(), static_cast<std::uint32_t>(planes.size())}}},
      MakeColorInfo(),
      MakeTimestamp(),
  };

  prototype.color.transfer = kMwStreamerColorTransferSmpte2084;
  CHECK_THROWS_AS(HostMatAdapter::ToBgr(prototype), std::invalid_argument);
  prototype.color.transfer = kMwStreamerColorTransferAribStdB67;
  CHECK_THROWS_AS(HostMatAdapter::ToBgr(prototype), std::invalid_argument);
  prototype.color.transfer = kMwStreamerColorTransferBt709;

  const cv::Mat wrong_type(kHeight, kWidth, CV_16UC3);
  CHECK_THROWS_AS(HostMatAdapter::FromBgr(wrong_type, prototype),
                  std::invalid_argument);
}

}  // namespace

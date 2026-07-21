#include <filesystem>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/standby_image.h"

namespace {

std::filesystem::path TestDataPath(const char* file_name) {
  return std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / file_name;
}

TEST(StandbyImageCpuIntegrationTest, DecodesAndScalesPngToNv12) {
  constexpr int kOutputWidth = 80;
  constexpr int kOutputHeight = 72;
  mw::streamer::ffmpeg::Frame frame =
      mw::streamer::internal::DecodeStandbyImageToNv12(
          TestDataPath("standby_black_160x144.png").string(), kOutputWidth,
          kOutputHeight, AVCOL_RANGE_MPEG, AVCOL_SPC_BT709);

  ASSERT_NE(frame.get(), nullptr);
  ASSERT_EQ(frame.get()->format, AV_PIX_FMT_NV12);
  ASSERT_EQ(frame.get()->width, kOutputWidth);
  ASSERT_EQ(frame.get()->height, kOutputHeight);
  ASSERT_NE(frame.get()->data[0], nullptr);
  ASSERT_NE(frame.get()->data[1], nullptr);
  EXPECT_EQ(frame.get()->color_range, AVCOL_RANGE_MPEG);
  EXPECT_EQ(frame.get()->colorspace, AVCOL_SPC_BT709);
  for (int row = 0; row < kOutputHeight; ++row) {
    for (int column = 0; column < kOutputWidth; ++column) {
      EXPECT_NEAR(frame.get()->data[0][row * frame.get()->linesize[0] + column],
                  16, 1);
    }
  }
  for (int row = 0; row < kOutputHeight / 2; ++row) {
    for (int column = 0; column < kOutputWidth; ++column) {
      EXPECT_NEAR(frame.get()->data[1][row * frame.get()->linesize[1] + column],
                  128, 1);
    }
  }
}

TEST(StandbyImageCpuIntegrationTest, RejectsUnsupportedVideoInput) {
  try {
    static_cast<void>(mw::streamer::internal::DecodeStandbyImageToNv12(
        TestDataPath("h264_160x144.mp4").string(), 160, 144, AVCOL_RANGE_MPEG,
        AVCOL_SPC_BT709));
    FAIL() << "expected unsupported input to fail";
  } catch (const mw::streamer::internal::Error& error) {
    EXPECT_EQ(error.code(), mw::streamer::StatusCode::kUnsupported);
  }
}

TEST(StandbyImageCpuIntegrationTest, RejectsNonPositiveOutputDimensions) {
  EXPECT_THROW(
      static_cast<void>(mw::streamer::internal::DecodeStandbyImageToNv12(
          TestDataPath("standby_black_160x144.png").string(), 0, 144,
          AVCOL_RANGE_MPEG, AVCOL_SPC_BT709)),
      mw::streamer::internal::Error);
}

}  // namespace

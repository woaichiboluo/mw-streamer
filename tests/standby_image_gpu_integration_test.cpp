#include <filesystem>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"
#include "mw/streamer/internal/standby_image.h"

namespace {

std::filesystem::path TestDataPath(const char* file_name) {
  return std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / file_name;
}

TEST(StandbyImageGpuIntegrationTest, UploadsOnceAndReturnsFrameReferences) {
  mw::streamer::internal::CudaDeviceContext device;
  try {
    device.Create(0);
  } catch (const mw::streamer::internal::Error& error) {
    if (error.code() == mw::streamer::StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kOutputWidth = 80;
  constexpr int kOutputHeight = 72;
  mw::streamer::internal::GpuNv12FramePool pool;
  pool.Create(device, kOutputWidth, kOutputHeight, 1);

  mw::streamer::internal::StandbyImage standby_image;
  EXPECT_FALSE(standby_image.is_loaded());
  standby_image.Load(TestDataPath("standby_black_160x144.png").string(), &pool,
                     AVCOL_RANGE_JPEG, AVCOL_SPC_BT709);
  EXPECT_TRUE(standby_image.is_loaded());

  mw::streamer::ffmpeg::Frame first = standby_image.Ref();
  mw::streamer::ffmpeg::Frame second = standby_image.Ref();
  ASSERT_NE(first.get(), second.get());
  EXPECT_EQ(first.get()->data[0], second.get()->data[0]);
  EXPECT_EQ(first.get()->data[1], second.get()->data[1]);
  EXPECT_EQ(first.get()->format, AV_PIX_FMT_CUDA);
  EXPECT_EQ(first.get()->width, kOutputWidth);
  EXPECT_EQ(first.get()->height, kOutputHeight);
  EXPECT_EQ(first.get()->color_range, AVCOL_RANGE_JPEG);
  EXPECT_EQ(first.get()->colorspace, AVCOL_SPC_BT709);

  mw::streamer::ffmpeg::Frame downloaded;
  downloaded.get()->format = AV_PIX_FMT_NV12;
  downloaded.get()->width = kOutputWidth;
  downloaded.get()->height = kOutputHeight;
  ASSERT_EQ(av_frame_get_buffer(downloaded.get(), 32), 0);
  ASSERT_EQ(av_hwframe_transfer_data(downloaded.get(), first.get(), 0), 0);
  EXPECT_NEAR(downloaded.get()->data[0][0], 0, 1);
  EXPECT_NEAR(downloaded.get()->data[1][0], 128, 1);
  EXPECT_NEAR(downloaded.get()->data[1][1], 128, 1);

  EXPECT_THROW(
      standby_image.Load(TestDataPath("standby_black_160x144.png").string(),
                         &pool, AVCOL_RANGE_JPEG, AVCOL_SPC_BT709),
      mw::streamer::internal::Error);
}

}  // namespace

#include "mw/streamer/internal/nv_decoder.h"

#include <cstdint>
#include <filesystem>
#include <optional>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/demuxer.h"

namespace mw::streamer::internal {
namespace {

struct DecodeFixture {
  const char* name;
  const char* file_name;
};

void ReceiveAvailable(NvDecoder* decoder, int* decoded_frames,
                      std::optional<std::int64_t>* previous_pts_us) {
  while (auto frame = decoder->Receive()) {
    const AVFrame* raw_frame = frame->get();
    ASSERT_NE(raw_frame, nullptr);
    EXPECT_EQ(raw_frame->format, AV_PIX_FMT_CUDA);
    EXPECT_EQ(raw_frame->width, 160);
    EXPECT_EQ(raw_frame->height, 144);
    EXPECT_NE(raw_frame->hw_frames_ctx, nullptr);

    const MwGpuNv12FrameView view = decoder->View(*frame);
    EXPECT_EQ(view.device_id, 0);
    EXPECT_EQ(view.width, 160);
    EXPECT_EQ(view.height, 144);
    EXPECT_NE(view.y, nullptr);
    EXPECT_NE(view.uv, nullptr);
    EXPECT_GE(view.y_pitch_bytes, 160);
    EXPECT_GE(view.uv_pitch_bytes, 160);
    EXPECT_EQ(view.pts, raw_frame->pts);
    EXPECT_EQ(view.time_base.numerator, raw_frame->time_base.num);
    EXPECT_EQ(view.time_base.denominator, raw_frame->time_base.den);
    if (previous_pts_us->has_value()) {
      EXPECT_GT(view.pts_us, **previous_pts_us);
    }
    *previous_pts_us = view.pts_us;
    ++*decoded_frames;
  }
}

class NvDecoderIntegrationTest : public testing::TestWithParam<DecodeFixture> {
};

TEST_P(NvDecoderIntegrationTest, DemuxesAndDecodesCudaNv12Frames) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / GetParam().file_name;
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));

  Demuxer demuxer;
  ASSERT_NO_THROW(demuxer.Open(input_path.string()));

  CudaDeviceContext device_context;
  try {
    device_context.Create(0);
  } catch (const Error& error) {
    if (error.code() == StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  NvDecoder decoder;
  ASSERT_NO_THROW(decoder.Open(demuxer.video_stream(), device_context));

  int decoded_frames = 0;
  std::optional<std::int64_t> previous_pts_us;
  while (auto packet = demuxer.Read()) {
    if (packet->get()->stream_index != decoder.stream_index()) {
      continue;
    }

    while (!decoder.Send(*packet)) {
      ReceiveAvailable(&decoder, &decoded_frames, &previous_pts_us);
    }
    ReceiveAvailable(&decoder, &decoded_frames, &previous_pts_us);
  }

  while (!decoder.SendEndOfStream()) {
    ReceiveAvailable(&decoder, &decoded_frames, &previous_pts_us);
  }
  ReceiveAvailable(&decoder, &decoded_frames, &previous_pts_us);
  EXPECT_EQ(decoded_frames, 5);
}

INSTANTIATE_TEST_SUITE_P(
    H264AndH265, NvDecoderIntegrationTest,
    testing::Values(DecodeFixture{"H264", "h264_160x144.mp4"},
                    DecodeFixture{"H265", "h265_160x144.mp4"}),
    [](const testing::TestParamInfo<DecodeFixture>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace mw::streamer::internal

#include "mw/streamer/internal/nv_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/aac_encoder.h"
#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/cuda_device_context.h"
#include "mw/streamer/internal/demuxer.h"
#include "mw/streamer/internal/fragmented_mp4_muxer.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"

namespace mw::streamer::internal {
namespace {

struct EncoderFixture {
  const char* name;
  AVCodecID codec_id;
  NvEncoderRateControl rate_control;
};

ffmpeg::Frame MakeSoftwareBlackFrame(int width, int height) {
  ffmpeg::Frame frame;
  frame.get()->format = AV_PIX_FMT_NV12;
  frame.get()->width = width;
  frame.get()->height = height;
  CheckFfmpeg(av_frame_get_buffer(frame.get(), 32),
              "failed to allocate a software NV12 test frame");
  frame.MakeWritable();
  for (int row = 0; row < height; ++row) {
    std::fill_n(frame.get()->data[0] + row * frame.get()->linesize[0], width,
                static_cast<std::uint8_t>(16));
  }
  for (int row = 0; row < height / 2; ++row) {
    std::fill_n(frame.get()->data[1] + row * frame.get()->linesize[1], width,
                static_cast<std::uint8_t>(128));
  }
  return frame;
}

void ReceiveAvailable(NvEncoder* encoder, int* packet_count,
                      bool* received_key_frame) {
  while (auto packet = encoder->Receive()) {
    EXPECT_GT(packet->get()->size, 0);
    EXPECT_EQ(packet->get()->time_base.num, encoder->time_base().num);
    EXPECT_EQ(packet->get()->time_base.den, encoder->time_base().den);
    if ((packet->get()->flags & AV_PKT_FLAG_KEY) != 0) {
      *received_key_frame = true;
    }
    ++*packet_count;
  }
}

void ReceiveToMuxer(NvEncoder* encoder, FragmentedMp4Muxer* muxer,
                    int* packet_count) {
  while (auto packet = encoder->Receive()) {
    muxer->WriteVideo(std::move(*packet));
    ++*packet_count;
  }
}

void ReceiveAudioToMuxer(AacEncoder* encoder, FragmentedMp4Muxer* muxer,
                         int* packet_count) {
  while (auto packet = encoder->Receive()) {
    muxer->WriteAudio(std::move(*packet));
    ++*packet_count;
  }
}

NormalizedAudioFrame MakeSilentAudioBlock() {
  NormalizedAudioFrame frame;
  frame.frame_count = kProcessorAudioBlockFrameCount;
  frame.samples.assign(
      kProcessorAudioBlockFrameCount * kProcessorAudioChannelCount, 0.0F);
  return frame;
}

NvEncoderConfig MakeEncoderConfig(AVCodecID codec_id,
                                  NvEncoderRateControl rate_control) {
  NvEncoderConfig config;
  config.codec_id = codec_id;
  config.width = 160;
  config.height = 144;
  config.frame_rate = {25, 1};
  config.gop_size = 5;
  config.bit_rate = 500'000;
  config.max_bit_rate = 500'000;
  config.vbv_buffer_size = 1'000'000;
  config.rate_control = rate_control;
  config.preset = "p4";
  config.color_range = AVCOL_RANGE_JPEG;
  config.color_space = AVCOL_SPC_BT709;
  return config;
}

class NvEncoderIntegrationTest : public testing::TestWithParam<EncoderFixture> {
};

TEST_P(NvEncoderIntegrationTest, EncodesCudaNv12Frames) {
  CudaDeviceContext device_context;
  try {
    device_context.Create(0);
  } catch (const Error& error) {
    if (error.code() == StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kWidth = 160;
  constexpr int kHeight = 144;
  GpuNv12FramePool frame_pool;
  ASSERT_NO_THROW(frame_pool.Create(device_context, kWidth, kHeight, 4));

  const NvEncoderConfig config =
      MakeEncoderConfig(GetParam().codec_id, GetParam().rate_control);

  NvEncoder encoder;
  ASSERT_NO_THROW(encoder.Open(config, device_context, frame_pool));
  ASSERT_TRUE(encoder.is_open());
  EXPECT_EQ(encoder.codec_parameters().get()->codec_id, GetParam().codec_id);
  EXPECT_EQ(encoder.codec_parameters().get()->width, kWidth);
  EXPECT_EQ(encoder.codec_parameters().get()->height, kHeight);
  EXPECT_EQ(encoder.codec_parameters().get()->color_range, AVCOL_RANGE_JPEG);
  EXPECT_EQ(encoder.codec_parameters().get()->color_space, AVCOL_SPC_BT709);
  EXPECT_GT(encoder.codec_parameters().get()->extradata_size, 0);

  ffmpeg::Frame software_frame = MakeSoftwareBlackFrame(kWidth, kHeight);
  {
    ffmpeg::Frame invalid_time_base_frame = frame_pool.Acquire();
    CheckFfmpeg(av_hwframe_transfer_data(invalid_time_base_frame.get(),
                                         software_frame.get(), 0),
                "failed to upload the invalid time base test frame");
    invalid_time_base_frame.get()->pts = 0;
    invalid_time_base_frame.get()->time_base = {1, 1'000'000};
    EXPECT_THROW(static_cast<void>(encoder.Send(invalid_time_base_frame)),
                 Error);
  }

  int packet_count = 0;
  bool received_key_frame = false;
  constexpr int kFrameCount = 6;
  for (int index = 0; index < kFrameCount; ++index) {
    ffmpeg::Frame gpu_frame = frame_pool.Acquire();
    CheckFfmpeg(
        av_hwframe_transfer_data(gpu_frame.get(), software_frame.get(), 0),
        "failed to upload the test frame");
    gpu_frame.get()->pts = index;
    gpu_frame.get()->time_base = encoder.time_base();
    while (!encoder.Send(gpu_frame)) {
      ReceiveAvailable(&encoder, &packet_count, &received_key_frame);
    }
    ReceiveAvailable(&encoder, &packet_count, &received_key_frame);
  }

  while (!encoder.SendEndOfStream()) {
    ReceiveAvailable(&encoder, &packet_count, &received_key_frame);
  }
  ReceiveAvailable(&encoder, &packet_count, &received_key_frame);

  EXPECT_EQ(packet_count, kFrameCount);
  EXPECT_TRUE(received_key_frame);
}

TEST_P(NvEncoderIntegrationTest, WritesVideoOnlyFragmentedMp4) {
  CudaDeviceContext device_context;
  try {
    device_context.Create(0);
  } catch (const Error& error) {
    if (error.code() == StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kWidth = 160;
  constexpr int kHeight = 144;
  GpuNv12FramePool frame_pool;
  frame_pool.Create(device_context, kWidth, kHeight, 4);
  const NvEncoderConfig config =
      MakeEncoderConfig(GetParam().codec_id, GetParam().rate_control);
  NvEncoder encoder;
  encoder.Open(config, device_context, frame_pool);

  const auto unique_suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      (std::string("mw-streamer-") + GetParam().name + "-" +
       std::to_string(unique_suffix) + ".mp4");

  FragmentedMp4Muxer muxer;
  ASSERT_NO_THROW(
      muxer.Open(output_path, encoder.codec_parameters(), encoder.time_base()));
  ffmpeg::Frame software_frame = MakeSoftwareBlackFrame(kWidth, kHeight);
  int packet_count = 0;
  constexpr int kFrameCount = 16;
  for (int index = 0; index < kFrameCount; ++index) {
    ffmpeg::Frame gpu_frame = frame_pool.Acquire();
    CheckFfmpeg(
        av_hwframe_transfer_data(gpu_frame.get(), software_frame.get(), 0),
        "failed to upload the test frame");
    gpu_frame.get()->pts = index;
    gpu_frame.get()->time_base = encoder.time_base();
    while (!encoder.Send(gpu_frame)) {
      ReceiveToMuxer(&encoder, &muxer, &packet_count);
    }
    ReceiveToMuxer(&encoder, &muxer, &packet_count);
  }
  while (!encoder.SendEndOfStream()) {
    ReceiveToMuxer(&encoder, &muxer, &packet_count);
  }
  ReceiveToMuxer(&encoder, &muxer, &packet_count);
  ASSERT_NO_THROW(muxer.Finish());

  EXPECT_EQ(packet_count, kFrameCount);
  ASSERT_TRUE(std::filesystem::is_regular_file(output_path));
  EXPECT_GT(std::filesystem::file_size(output_path), 0U);

  Demuxer demuxer;
  ASSERT_NO_THROW(demuxer.Open(output_path.string()));
  EXPECT_EQ(demuxer.video_stream().codec_parameters.get()->codec_id,
            GetParam().codec_id);
  EXPECT_FALSE(demuxer.has_audio());

  std::ifstream input(output_path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(bytes.find("moov"), std::string::npos);
  EXPECT_NE(bytes.find("moof"), std::string::npos);
  EXPECT_NE(bytes.find("mdat"), std::string::npos);

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

TEST_P(NvEncoderIntegrationTest, WritesVideoAndAacFragmentedMp4) {
  CudaDeviceContext device_context;
  try {
    device_context.Create(0);
  } catch (const Error& error) {
    if (error.code() == StatusCode::kUnavailable) {
      GTEST_SKIP() << error.what();
    }
    throw;
  }

  constexpr int kWidth = 160;
  constexpr int kHeight = 144;
  GpuNv12FramePool frame_pool;
  frame_pool.Create(device_context, kWidth, kHeight, 4);
  const NvEncoderConfig config =
      MakeEncoderConfig(GetParam().codec_id, GetParam().rate_control);
  NvEncoder video_encoder;
  video_encoder.Open(config, device_context, frame_pool);
  AacEncoder audio_encoder;
  audio_encoder.Open(128'000);

  const auto unique_suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      (std::string("mw-streamer-av-") + GetParam().name + "-" +
       std::to_string(unique_suffix) + ".mp4");

  FragmentedMp4Muxer muxer;
  ASSERT_NO_THROW(muxer.Open(
      output_path, video_encoder.codec_parameters(), video_encoder.time_base(),
      &audio_encoder.codec_parameters(), audio_encoder.time_base()));

  ffmpeg::Frame software_frame = MakeSoftwareBlackFrame(kWidth, kHeight);
  int video_packet_count = 0;
  constexpr int kVideoFrameCount = 16;
  for (int index = 0; index < kVideoFrameCount; ++index) {
    ffmpeg::Frame gpu_frame = frame_pool.Acquire();
    CheckFfmpeg(
        av_hwframe_transfer_data(gpu_frame.get(), software_frame.get(), 0),
        "failed to upload the test frame");
    gpu_frame.get()->pts = index;
    gpu_frame.get()->time_base = video_encoder.time_base();
    while (!video_encoder.Send(gpu_frame)) {
      ReceiveToMuxer(&video_encoder, &muxer, &video_packet_count);
    }
    ReceiveToMuxer(&video_encoder, &muxer, &video_packet_count);
  }
  while (!video_encoder.SendEndOfStream()) {
    ReceiveToMuxer(&video_encoder, &muxer, &video_packet_count);
  }
  ReceiveToMuxer(&video_encoder, &muxer, &video_packet_count);

  int audio_packet_count = 0;
  constexpr int kAudioBlockCount = 30;
  const NormalizedAudioFrame audio_block = MakeSilentAudioBlock();
  for (int index = 0; index < kAudioBlockCount; ++index) {
    while (!audio_encoder.Send(audio_block)) {
      ReceiveAudioToMuxer(&audio_encoder, &muxer, &audio_packet_count);
    }
    ReceiveAudioToMuxer(&audio_encoder, &muxer, &audio_packet_count);
  }
  while (!audio_encoder.SendEndOfStream()) {
    ReceiveAudioToMuxer(&audio_encoder, &muxer, &audio_packet_count);
  }
  ReceiveAudioToMuxer(&audio_encoder, &muxer, &audio_packet_count);
  ASSERT_NO_THROW(muxer.Finish());

  EXPECT_EQ(video_packet_count, kVideoFrameCount);
  EXPECT_GT(audio_packet_count, 0);
  {
    Demuxer demuxer;
    ASSERT_NO_THROW(demuxer.Open(output_path.string()));
    EXPECT_EQ(demuxer.video_stream().codec_parameters.get()->codec_id,
              GetParam().codec_id);
    ASSERT_TRUE(demuxer.has_audio());
    EXPECT_EQ(demuxer.audio_stream().codec_parameters.get()->codec_id,
              AV_CODEC_ID_AAC);
  }

  std::error_code ignored;
  std::filesystem::remove(output_path, ignored);
}

INSTANTIATE_TEST_SUITE_P(
    CodecAndRateControl, NvEncoderIntegrationTest,
    testing::Values(
        EncoderFixture{"H264Cbr", AV_CODEC_ID_H264, NvEncoderRateControl::kCbr},
        EncoderFixture{"H264Vbr", AV_CODEC_ID_H264, NvEncoderRateControl::kVbr},
        EncoderFixture{"H265Cbr", AV_CODEC_ID_HEVC, NvEncoderRateControl::kCbr},
        EncoderFixture{"H265Vbr", AV_CODEC_ID_HEVC,
                       NvEncoderRateControl::kVbr}),
    [](const testing::TestParamInfo<EncoderFixture>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace mw::streamer::internal

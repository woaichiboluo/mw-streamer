#include "mw/streamer/internal/demuxer.h"

#include <filesystem>
#include <string>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

std::filesystem::path TestDataPath(const char* file_name) {
  return std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) / file_name;
}

TEST(DemuxerIntegrationTest, ReadsSelectedPacketsUntilCleanEndOfStream) {
  Demuxer demuxer;
  ASSERT_NO_THROW(demuxer.Open(TestDataPath("h264_160x144.mp4").string()));
  EXPECT_EQ(demuxer.video_stream().codec_parameters.get()->codec_id,
            AV_CODEC_ID_H264);
  EXPECT_FALSE(demuxer.has_audio());

  int video_packets = 0;
  while (auto packet = demuxer.Read()) {
    EXPECT_EQ(packet->get()->stream_index, demuxer.video_stream().index);
    EXPECT_EQ(packet->get()->time_base.num,
              demuxer.video_stream().time_base.num);
    EXPECT_EQ(packet->get()->time_base.den,
              demuxer.video_stream().time_base.den);
    ++video_packets;
  }
  EXPECT_GT(video_packets, 0);
}

TEST(DemuxerIntegrationTest, ReadsCompletePipelineFileFixture) {
  Demuxer demuxer;
  ASSERT_NO_THROW(
      demuxer.Open(TestDataPath("h264_aac_mono_44100_1s.mp4").string()));

  const AVCodecParameters* video =
      demuxer.video_stream().codec_parameters.get();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->codec_id, AV_CODEC_ID_H264);
  EXPECT_EQ(video->width, 160);
  EXPECT_EQ(video->height, 144);
  EXPECT_EQ(demuxer.video_stream().frame_rate.num, 5);
  EXPECT_EQ(demuxer.video_stream().frame_rate.den, 1);

  ASSERT_TRUE(demuxer.has_audio());
  const AVCodecParameters* audio =
      demuxer.audio_stream().codec_parameters.get();
  ASSERT_NE(audio, nullptr);
  EXPECT_EQ(audio->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(audio->sample_rate, 44'100);
  EXPECT_EQ(audio->ch_layout.nb_channels, 1);

  int video_packets = 0;
  int audio_packets = 0;
  while (auto packet = demuxer.Read()) {
    if (packet->get()->stream_index == demuxer.video_stream().index) {
      ++video_packets;
    } else {
      ASSERT_EQ(packet->get()->stream_index, demuxer.audio_stream().index);
      ++audio_packets;
    }
  }
  EXPECT_EQ(video_packets, 5);
  EXPECT_EQ(audio_packets, 45);
}

TEST(DemuxerIntegrationTest, FailedOpenLeavesObjectReusable) {
  Demuxer demuxer;
  EXPECT_THROW(
      demuxer.Open(TestDataPath("file-that-does-not-exist.mp4").string()),
      Error);
  EXPECT_FALSE(demuxer.is_open());

  EXPECT_NO_THROW(demuxer.Open(TestDataPath("h265_160x144.mp4").string()));
  EXPECT_TRUE(demuxer.is_open());
}

TEST(DemuxerIntegrationTest, CancelledOpenIsUnavailable) {
  FfmpegInterrupt interrupt;
  interrupt.Cancel();
  DemuxerOpenOptions options;
  options.interrupt = &interrupt;

  Demuxer demuxer;
  try {
    demuxer.Open(TestDataPath("file-that-does-not-exist.mp4").string(),
                 options);
    FAIL() << "Demuxer::Open returned";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kUnavailable);
    EXPECT_NE(std::string(error.what()).find("cancelled or timed out"),
              std::string::npos);
  }
  EXPECT_FALSE(demuxer.is_open());
}

}  // namespace
}  // namespace mw::streamer::internal

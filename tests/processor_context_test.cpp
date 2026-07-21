#include "mw/streamer/internal/processor_context.h"

#include <utility>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include "mw/streamer/internal/stream_info.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {
namespace {

StreamInfo MakeVideoStream() {
  StreamInfo stream;
  stream.index = 2;
  stream.frame_rate = {25, 1};
  stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  stream.codec_parameters.get()->width = 1'920;
  stream.codec_parameters.get()->height = 1'080;
  stream.codec_parameters.get()->color_range = AVCOL_RANGE_UNSPECIFIED;
  stream.codec_parameters.get()->color_space = AVCOL_SPC_BT709;
  return stream;
}

StreamInfo MakeAudioStream() {
  StreamInfo stream;
  stream.index = 3;
  stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  stream.codec_parameters.get()->sample_rate = 44'100;
  av_channel_layout_default(&stream.codec_parameters.get()->ch_layout, 1);
  return stream;
}

TEST(ProcessorContextTest, MapsOriginalInputAndFixedOutputInformation) {
  StreamInfo video = MakeVideoStream();
  StreamInfo audio = MakeAudioStream();
  MwInputSourceInfo source = MakeInputSourceInfo(0, video, &audio);

  EXPECT_EQ(source.input_index, 0U);
  EXPECT_EQ(source.video.present, 1);
  EXPECT_EQ(source.video.width, 1'920);
  EXPECT_EQ(source.video.height, 1'080);
  EXPECT_EQ(source.video.frame_rate.numerator, 25);
  EXPECT_EQ(source.video.color_range, MW_VIDEO_COLOR_RANGE_UNSPECIFIED);
  EXPECT_EQ(source.video.color_space, MW_VIDEO_COLOR_SPACE_BT709);
  EXPECT_EQ(source.audio.present, 1);
  EXPECT_EQ(source.audio.sample_rate, 44'100);
  EXPECT_EQ(source.audio.channel_count, 1);

  VideoEncoderConfig video_encoder;
  video_encoder.width = 1'280;
  video_encoder.height = 720;
  video_encoder.frame_rate = {50, 1};
  const std::vector<MwInputSourceInfo> sources{source};
  const MwProcessorContext context =
      MakeProcessorContext(1, sources, 0, video_encoder, true);
  EXPECT_EQ(context.device_id, 1);
  EXPECT_EQ(context.inputs, sources.data());
  EXPECT_EQ(context.input_count, 1U);
  EXPECT_EQ(context.video_output.width, 1'280);
  EXPECT_EQ(context.video_output.height, 720);
  EXPECT_EQ(context.video_output.frame_rate.numerator, 50);
  EXPECT_EQ(context.video_output.color_range, MW_VIDEO_COLOR_RANGE_LIMITED);
  EXPECT_EQ(context.video_output.color_space, MW_VIDEO_COLOR_SPACE_BT709);
  EXPECT_EQ(context.audio_output.sample_rate, 48'000);
  EXPECT_EQ(context.audio_output.channel_count, 2);
  EXPECT_EQ(context.audio_output.block_frame_count, 1'024U);
}

TEST(ProcessorContextTest, KeepsAudioOutputAbsentWhenNoInputHasAudio) {
  StreamInfo video = MakeVideoStream();
  const std::vector<MwInputSourceInfo> sources{
      MakeInputSourceInfo(0, video, nullptr)};
  VideoEncoderConfig video_encoder;
  video_encoder.width = 1'920;
  video_encoder.height = 1'080;
  video_encoder.frame_rate = {25, 1};

  const MwProcessorContext context =
      MakeProcessorContext(0, sources, 0, video_encoder, false);
  EXPECT_EQ(context.audio_output.sample_rate, 0);
  EXPECT_EQ(context.audio_output.channel_count, 0);
  EXPECT_EQ(context.audio_output.block_frame_count, 0U);
}

}  // namespace
}  // namespace mw::streamer::internal

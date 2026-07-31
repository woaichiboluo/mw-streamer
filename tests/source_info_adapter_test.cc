#include "mw/processor/internal/source_info_adapter.h"

#include <array>
#include <optional>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/media/internal/codec_bridge.h"

namespace {

using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::media::internal::ToAvCodecId;
using mw::streamer::media::internal::ToMwStreamerCodec;
using mw::streamer::processor::internal::MakeProcessorSourceInfo;

StreamInfo MakeVideoStream(AVCodecID codec_id) {
  StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = {1, 90000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = codec_id;
  parameters->width = 1920;
  parameters->height = 1080;
  parameters->framerate = {30000, 1001};
  return stream;
}

StreamInfo MakeAudioStream(AVCodecID codec_id) {
  StreamInfo stream;
  stream.stream_index = 1;
  stream.time_base = {1, 48000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = codec_id;
  parameters->sample_rate = 48000;
  av_channel_layout_default(&parameters->ch_layout, 2);
  return stream;
}

}  // namespace

TEST_CASE("媒体Codec Bridge双向映射公开编码") {
  const std::array cases = {
      std::pair{kMwStreamerCodecH264, AV_CODEC_ID_H264},
      std::pair{kMwStreamerCodecH265, AV_CODEC_ID_HEVC},
      std::pair{kMwStreamerCodecAv1, AV_CODEC_ID_AV1},
      std::pair{kMwStreamerCodecAac, AV_CODEC_ID_AAC},
      std::pair{kMwStreamerCodecG711A, AV_CODEC_ID_PCM_ALAW},
      std::pair{kMwStreamerCodecG711U, AV_CODEC_ID_PCM_MULAW},
      std::pair{kMwStreamerCodecOpus, AV_CODEC_ID_OPUS},
      std::pair{kMwStreamerCodecMjpeg, AV_CODEC_ID_MJPEG},
      std::pair{kMwStreamerCodecVp8, AV_CODEC_ID_VP8},
      std::pair{kMwStreamerCodecVp9, AV_CODEC_ID_VP9},
  };

  for (const auto& [mw_codec, av_codec] : cases) {
    CHECK(ToMwStreamerCodec(av_codec) == mw_codec);
    CHECK(ToAvCodecId(mw_codec) == av_codec);
  }
  CHECK(ToMwStreamerCodec(AV_CODEC_ID_FLAC) == kMwStreamerCodecUnknown);
  CHECK(ToAvCodecId(kMwStreamerCodecUnknown) == AV_CODEC_ID_NONE);
}

TEST_CASE("SourceInfoAdapter映射视频编码和基础信息") {
  const std::array cases = {
      std::pair{AV_CODEC_ID_H264, kMwStreamerCodecH264},
      std::pair{AV_CODEC_ID_HEVC, kMwStreamerCodecH265},
      std::pair{AV_CODEC_ID_AV1, kMwStreamerCodecAv1},
      std::pair{AV_CODEC_ID_MJPEG, kMwStreamerCodecMjpeg},
      std::pair{AV_CODEC_ID_VP8, kMwStreamerCodecVp8},
      std::pair{AV_CODEC_ID_VP9, kMwStreamerCodecVp9},
  };

  for (const auto& [codec_id, expected] : cases) {
    const auto source_info =
        MakeProcessorSourceInfo(std::nullopt, MakeVideoStream(codec_id));
    CHECK(source_info.has_audio == 0);
    CHECK(source_info.has_video == 1);
    CHECK(source_info.video.codec == expected);
    CHECK(source_info.video.width == 1920);
    CHECK(source_info.video.height == 1080);
    CHECK(source_info.video.frame_rate.num == 30000);
    CHECK(source_info.video.frame_rate.den == 1001);
    CHECK(source_info.video.time_base.num == 1);
    CHECK(source_info.video.time_base.den == 90000);
  }
}

TEST_CASE("SourceInfoAdapter使用0/1表示未知视频帧率") {
  auto stream = MakeVideoStream(AV_CODEC_ID_H264);
  stream.codec_parameters.get()->framerate = {0, 0};

  const auto source_info =
      MakeProcessorSourceInfo(std::nullopt, std::move(stream));

  CHECK(source_info.video.frame_rate.num == 0);
  CHECK(source_info.video.frame_rate.den == 1);
}

TEST_CASE("SourceInfoAdapter映射音频编码和基础信息") {
  const std::array cases = {
      std::pair{AV_CODEC_ID_AAC, kMwStreamerCodecAac},
      std::pair{AV_CODEC_ID_PCM_ALAW, kMwStreamerCodecG711A},
      std::pair{AV_CODEC_ID_PCM_MULAW, kMwStreamerCodecG711U},
      std::pair{AV_CODEC_ID_OPUS, kMwStreamerCodecOpus},
  };

  for (const auto& [codec_id, expected] : cases) {
    const auto source_info =
        MakeProcessorSourceInfo(MakeAudioStream(codec_id), std::nullopt);
    CHECK(source_info.has_audio == 1);
    CHECK(source_info.has_video == 0);
    CHECK(source_info.audio.codec == expected);
    CHECK(source_info.audio.sample_rate == 48000);
    CHECK(source_info.audio.channel_count == 2);
    CHECK(source_info.audio.time_base.num == 1);
    CHECK(source_info.audio.time_base.den == 48000);
  }
}

TEST_CASE("SourceInfoAdapter保留未知编码和音视频轨道集合") {
  const auto source_info = MakeProcessorSourceInfo(
      MakeAudioStream(AV_CODEC_ID_FLAC), MakeVideoStream(AV_CODEC_ID_RAWVIDEO));

  CHECK(source_info.has_audio == 1);
  CHECK(source_info.has_video == 1);
  CHECK(source_info.audio.codec == kMwStreamerCodecUnknown);
  CHECK(source_info.video.codec == kMwStreamerCodecUnknown);
}

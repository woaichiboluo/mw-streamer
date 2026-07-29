#include <catch2/catch_test_macros.hpp>
#include <string>

#include "mw/decoder/config.h"

namespace {

using mw::streamer::decoder::AudioDecoderConfig;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::decoder::VideoDecoderConfig;

TEST_CASE("Decoder配置默认使用FFmpeg自动选择和CUDA设备零") {
  const AudioDecoderConfig audio;
  const VideoDecoderConfig video;

  CHECK(audio.decoder_name.empty());
  CHECK(video.decoder_name.empty());
  CHECK(video.backend == VideoDecoderBackend::kCuda);
  CHECK(video.device_index == 0);
}

TEST_CASE("Decoder配置支持指定解码器和CUDA视频解码") {
  AudioDecoderConfig audio;
  audio.decoder_name = "libopus";

  VideoDecoderConfig video;
  video.decoder_name = "h264";
  video.backend = VideoDecoderBackend::kCuda;
  video.device_index = 2;

  const auto audio_copy = audio;
  const auto video_copy = video;
  CHECK(audio_copy.decoder_name == "libopus");
  CHECK(video_copy.decoder_name == "h264");
  CHECK(video_copy.backend == VideoDecoderBackend::kCuda);
  CHECK(video_copy.device_index == 2);
}

}  // namespace

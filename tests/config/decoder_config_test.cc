#include <catch2/catch_test_macros.hpp>
#include <string>

#include "mw/decoder/config.h"
#include "mw/processor/config.h"

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

TEST_CASE("Processor配置深拷贝保留业务字符串") {
  mw::streamer::processor::StreamingProcessorConfig original;
  original.output_width = 1920;
  original.output_height = 1080;
  original.config = "model = 'a'";
  const auto copy = original;
  original.config.clear();
  CHECK(copy.output_width == 1920);
  CHECK(copy.output_height == 1080);
  CHECK(copy.config == "model = 'a'");
  mw::streamer::processor::FileProcessorConfig file;
  file.config = "model = 'file'";
  const auto file_copy = file;
  file.config.clear();
  CHECK(file_copy.config == "model = 'file'");
}

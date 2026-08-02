#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <type_traits>

#include "mw/pipeline/config.h"

namespace {

using mw::streamer::pipeline::LocalFilePipelineConfig;
using mw::streamer::pipeline::StreamingPipelineConfig;
using mw::streamer::processor::FileProcessorConfig;
using mw::streamer::processor::StreamingProcessorConfig;

static_assert(std::is_copy_constructible_v<StreamingPipelineConfig>);
static_assert(std::is_move_constructible_v<StreamingPipelineConfig>);
static_assert(std::is_copy_constructible_v<LocalFilePipelineConfig>);
static_assert(std::is_move_constructible_v<LocalFilePipelineConfig>);

}  // namespace

TEST_CASE("streaming pipeline config owns the complete streaming setup") {
  StreamingPipelineConfig config;

  CHECK(config.input_url.empty());
  CHECK(config.zlm.player.connect_timeout == std::chrono::seconds(10));
  CHECK(config.zlm.player.media_timeout == std::chrono::seconds(5));
  CHECK(config.zlm.player.local_bind_ip.empty());
  CHECK(config.zlm.output.pusher.connect_timeout == std::chrono::seconds(10));
  CHECK(config.zlm.output.pusher.local_bind_ip.empty());
  CHECK(config.zlm.output.muxer.paced_sender_interval ==
        std::chrono::milliseconds::zero());
  CHECK(config.zlm.output.recording.file_buffer_size == 64 * 1024);
  CHECK(config.zlm.output.recording.hls_segment_duration ==
        std::chrono::seconds(2));
  CHECK(config.reconnect_policy.max_retries == -1);
  CHECK(config.cache_duration == std::chrono::milliseconds::zero());
  CHECK(config.audio_queue_capacity == 256);
  CHECK(config.video_queue_capacity == 128);
  CHECK(config.max_track_wait == std::chrono::milliseconds(500));
  CHECK(config.video_decoder.backend ==
        mw::streamer::decoder::VideoDecoderBackend::kCuda);
  CHECK(config.processor.output_width == 0);
  CHECK(config.processor.output_height == 0);
  CHECK(config.video_encoder.codec == kMwStreamerCodecH264);
  CHECK_FALSE(config.standby.enabled);
  CHECK(config.standby.image_path.empty());
  CHECK(config.output_targets.empty());
}

TEST_CASE("local-file pipeline config excludes streaming-only setup") {
  LocalFilePipelineConfig config;

  CHECK(config.input_path.empty());
  CHECK(config.video_decoder.backend ==
        mw::streamer::decoder::VideoDecoderBackend::kCuda);
  CHECK(config.processor.config.empty());
}

TEST_CASE("processor config owns its opaque string across copies") {
  StreamingProcessorConfig original;
  original.output_width = 1920;
  original.output_height = 1080;
  original.config = R"({"model":"a"})";

  StreamingProcessorConfig copy = original;
  original.config = R"({"model":"b"})";

  CHECK(copy.output_width == 1920);
  CHECK(copy.output_height == 1080);
  CHECK(copy.config == R"({"model":"a"})");

  FileProcessorConfig file;
  file.config = R"({"model":"file"})";
  const FileProcessorConfig file_copy = file;
  file.config.clear();
  CHECK(file_copy.config == R"({"model":"file"})");
}

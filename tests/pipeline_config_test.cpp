#include "mw/streamer/pipeline_config.h"

#include <chrono>

#include "gtest/gtest.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/pipeline_config_validator.h"

namespace mw::streamer::internal {
namespace {

PipelineConfig MakeValidConfig() {
  PipelineConfig config;
  config.id = "pipeline";
  InputConfig input;
  input.id = "camera";
  input.protocol = InputProtocol::kRtsp;
  input.url = "rtsp://127.0.0.1:8554/camera";
  config.inputs.push_back(input);
  OutputConfig output;
  output.id = "recording";
  output.protocol = OutputProtocol::kFile;
  output.url = "output.mp4";
  config.outputs.push_back(output);
  config.video_encoder.width = 1'920;
  config.video_encoder.height = 1'080;
  config.video_encoder.frame_rate = {25, 1};
  config.standby_image_path = "standby.png";
  return config;
}

TEST(PipelineConfigTest, AcceptsCompleteStaticConfiguration) {
  const PipelineConfig config = MakeValidConfig();
  EXPECT_NO_THROW(ValidatePipelineConfig(config));
}

TEST(PipelineConfigTest, RejectsDuplicateEndpointIdsAcrossInputsAndOutputs) {
  PipelineConfig config = MakeValidConfig();
  config.outputs[0].id = config.inputs[0].id;
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsRtspTransportOnAnotherProtocol) {
  PipelineConfig config = MakeValidConfig();
  config.inputs[0].protocol = InputProtocol::kSrt;
  config.inputs[0].rtsp_transport = RtspTransport::kUdp;
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, AcceptsExplicitHlsModes) {
  PipelineConfig config = MakeValidConfig();
  config.inputs[0].protocol = InputProtocol::kHls;
  config.inputs[0].hls_mode = HlsMode::kLive;
  EXPECT_NO_THROW(ValidatePipelineConfig(config));

  config.inputs[0].hls_mode = HlsMode::kVod;
  EXPECT_NO_THROW(ValidatePipelineConfig(config));
}

TEST(PipelineConfigTest, RejectsHlsVodModeForAnotherProtocol) {
  PipelineConfig config = MakeValidConfig();
  config.inputs[0].hls_mode = HlsMode::kVod;
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsUnknownHlsMode) {
  PipelineConfig config = MakeValidConfig();
  config.inputs[0].protocol = InputProtocol::kHls;
  config.inputs[0].hls_mode = static_cast<HlsMode>(-1);
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, AllowsLiveDelayOnlyWhenEveryInputIsLive) {
  PipelineConfig config = MakeValidConfig();
  config.live_delay = std::chrono::milliseconds(100);
  config.inputs[0].protocol = InputProtocol::kHls;
  config.inputs[0].hls_mode = HlsMode::kLive;
  EXPECT_NO_THROW(ValidatePipelineConfig(config));

  config.inputs.push_back(config.inputs[0]);
  config.inputs.back().id = "recorded-camera";
  config.inputs.back().hls_mode = HlsMode::kVod;
  EXPECT_THROW(ValidatePipelineConfig(config), Error);

  config.inputs.erase(config.inputs.begin());
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsInvalidReconnectRange) {
  PipelineConfig config = MakeValidConfig();
  config.inputs[0].reconnect.initial_backoff = std::chrono::seconds(30);
  config.inputs[0].reconnect.maximum_backoff = std::chrono::seconds(1);
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsUnsupportedAudioShape) {
  PipelineConfig config = MakeValidConfig();
  config.audio_encoder.channel_count = 1;
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsMissingRequiredOutputSpecification) {
  PipelineConfig config = MakeValidConfig();
  config.video_encoder.frame_rate = {0, 1};
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

TEST(PipelineConfigTest, RejectsNegativeOutputTimeouts) {
  PipelineConfig config = MakeValidConfig();
  config.outputs[0].open_timeout = std::chrono::milliseconds(-1);
  EXPECT_THROW(ValidatePipelineConfig(config), Error);

  config.outputs[0].open_timeout = std::chrono::milliseconds(0);
  config.outputs[0].write_timeout = std::chrono::milliseconds(-1);
  EXPECT_THROW(ValidatePipelineConfig(config), Error);
}

}  // namespace
}  // namespace mw::streamer::internal

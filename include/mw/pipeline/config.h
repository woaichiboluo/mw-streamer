#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_CONFIG_H_

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "mw/decoder/config.h"
#include "mw/encoder/config.h"
#include "mw/input/config.h"
#include "mw/processor/config.h"
#include "mw/zlm/config.h"

namespace mw::streamer::pipeline {

struct StreamingStandbyConfig {
  bool enabled = false;

  // Empty uses the built-in loading image. A non-empty path is decoded once
  // after the first processed video frame determines the output frame format.
  std::string image_path;
};

struct StreamingPipelineConfig {
  // PlayerProxy accepts network URLs and finite inputs supported by ZLM.
  std::string input_url;
  zlm::PipelineConfig zlm;
  input::ReconnectPolicy reconnect_policy;
  std::chrono::milliseconds cache_duration{0};
  std::size_t audio_queue_capacity = 256;
  std::size_t video_queue_capacity = 128;

  // Wait for a temporarily missing processed track before synthesizing
  // silence or repeating video. Normal output is not delayed while both
  // tracks are available.
  std::chrono::milliseconds max_track_wait{500};

  decoder::AudioDecoderConfig audio_decoder;
  decoder::VideoDecoderConfig video_decoder;
  processor::StreamingProcessorConfig processor;
  encoder::AudioEncoderConfig audio_encoder;
  encoder::VideoEncoderConfig video_encoder;
  StreamingStandbyConfig standby;

  // Optional source-packet outputs. Targets use the same URL/path semantics
  // and file naming rules as OutputSession. Empty disables source recording
  // and relay without changing the processed output pipeline.
  std::vector<std::string> input_targets;

  // RTMP, RTSP, SRT, fragmented MP4, and HLS-fMP4 targets accepted by
  // OutputSession. Encoded StreamInfo is produced at runtime and deliberately
  // does not belong to this configuration.
  std::vector<std::string> output_targets;
};

struct RemuxPipelineConfig {
  // RemuxPipeline preserves compressed audio/video and does not decode,
  // process, or re-encode it.
  std::string input_url;
  zlm::PipelineConfig zlm;
  input::ReconnectPolicy reconnect_policy;

  // RTMP, RTSP, SRT, fragmented MP4, and HLS-fMP4 targets accepted by
  // OutputSession. At least one target is required.
  std::vector<std::string> output_targets;
};

struct LocalFilePipelineConfig {
  // Local-file processing uses FFmpeg demuxing directly and runs without
  // PacketQueue pacing, encoding, or OutputSession.
  std::string input_path;

  decoder::AudioDecoderConfig audio_decoder;
  decoder::VideoDecoderConfig video_decoder;
  processor::FileProcessorConfig processor;
};

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_CONFIG_H_

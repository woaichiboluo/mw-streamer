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

struct StreamingPipelineConfig {
  // PlayerProxy accepts network URLs and finite inputs supported by ZLM.
  std::string input_url;
  zlm::PipelineConfig zlm;
  input::ReconnectPolicy reconnect_policy;
  std::chrono::milliseconds cache_duration{0};
  std::size_t audio_queue_capacity = 256;
  std::size_t video_queue_capacity = 128;
  decoder::AudioDecoderConfig audio_decoder;
  decoder::VideoDecoderConfig video_decoder;
  processor::StreamingProcessorConfig processor;
  encoder::AudioEncoderConfig audio_encoder;
  encoder::VideoEncoderConfig video_encoder;

  // RTMP, RTSP, SRT, fragmented MP4, and HLS-fMP4 targets accepted by
  // OutputSession. Encoded StreamInfo is produced at runtime and deliberately
  // does not belong to this configuration.
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

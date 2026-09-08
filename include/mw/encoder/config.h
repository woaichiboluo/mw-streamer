#ifndef MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_

#include <cstddef>
#include <map>
#include <string>

#include "mw/media/types.h"

namespace mw::streamer::encoder {

using EncoderProperties = std::map<std::string, std::string>;

struct AudioEncoderConfig {
  // Empty selects FFmpeg's default AAC encoder.
  std::string encoder_name;
  EncoderProperties properties;
};

struct VideoEncoderConfig {
  MwStreamerCodec codec = kMwStreamerCodecH264;
  // Empty selects NVENC for CUDA frames and FFmpeg's default encoder for
  // host frames.
  std::string encoder_name;
  MwStreamerRational frame_rate{0, 1};
  EncoderProperties properties;
};

struct EncoderSinkConfig {
  encoder::AudioEncoderConfig audio_encoder;
  encoder::VideoEncoderConfig video_encoder;
  // Positive limits. Lifecycle notifications do not consume frame quota.
  std::size_t frame_queue_capacity = 256;
  // Encoded packets retained until all declared tracks have opened encoders.
  std::size_t startup_packet_capacity = 256;
};

}  // namespace mw::streamer::encoder

#endif  // MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_

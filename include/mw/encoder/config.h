#ifndef MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_

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

}  // namespace mw::streamer::encoder

#endif  // MW_STREAMER_INCLUDE_MW_ENCODER_CONFIG_H_

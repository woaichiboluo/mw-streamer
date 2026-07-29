#ifndef MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_

#include <string>

namespace mw::streamer::decoder {

enum class VideoDecoderBackend {
  kSoftware,
  kCuda,
};

struct AudioDecoderConfig {
  // Empty selects FFmpeg's default decoder for the input codec.
  std::string decoder_name;
};

struct VideoDecoderConfig {
  // Empty selects FFmpeg's default decoder for the input codec.
  std::string decoder_name;
  VideoDecoderBackend backend = VideoDecoderBackend::kCuda;
  // Used by the CUDA backend and must be non-negative.
  int device_index = 0;
};

}  // namespace mw::streamer::decoder

#endif  // MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_

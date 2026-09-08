#ifndef MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_

#include <chrono>
#include <cstddef>
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

struct DecoderSinkConfig {
  // Zero forwards immediately; otherwise one to thirty seconds, inclusive.
  std::chrono::milliseconds cache_duration{0};
  // Maximum queued packets awaiting decoding per track; must be positive.
  // These limits exclude the upstream time cache and lifecycle messages.
  std::size_t audio_decode_queue_capacity = 256;
  std::size_t video_decode_queue_capacity = 128;
  decoder::AudioDecoderConfig audio_decoder;
  decoder::VideoDecoderConfig video_decoder;
};

}  // namespace mw::streamer::decoder

#endif  // MW_STREAMER_INCLUDE_MW_DECODER_CONFIG_H_

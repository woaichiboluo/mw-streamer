#ifndef MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_CONFIG_H_

#include <chrono>
#include <cstddef>
#include <string>

namespace mw::streamer::synchronizer {

struct SynchronizerSinkConfig {
  // Positive per-track limits for ingress and scheduled frames separately.
  // Saturation discards older raw frames; producers never wait for capacity.
  std::size_t frame_queue_capacity = 128;
  // Common audio/video playout buffer and maximum source-frame age at a slot.
  // Release each slot this long after its source deadline so a small arrival
  // delay does not immediately synthesize silence or repeat the previous frame.
  // This adds fixed wall-clock latency, never an audio/video PTS offset.
  std::chrono::milliseconds max_frame_lateness{100};
  std::chrono::milliseconds standby_timeout{500};
  // Empty selects the existing built-in loading image.
  std::string standby_image_path;
};

}  // namespace mw::streamer::synchronizer

#endif  // MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_CONFIG_H_

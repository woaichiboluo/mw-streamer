#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_CONFIG_H_

#include <cstddef>
#include <string>

#include "mw/zlm/config.h"

namespace mw::streamer {

struct RemuxSinkConfig {
  // Exactly one RTMP/RTSP/SRT URL, fragmented MP4 path, or HLS-fMP4 path.
  std::string target;
  OutputConfig zlm;
  // Positive limit for the delivery queue and, separately, the startup cache.
  // Ordered lifecycle notifications use no quota.
  std::size_t packet_queue_capacity = 384;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_CONFIG_H_

#ifndef MW_STREAMER_OUTPUT_CONFIG_H_
#define MW_STREAMER_OUTPUT_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mw/streamer/zlm/config.h"

namespace mw::streamer {

struct RemuxSinkConfig {
  // Exactly one RTMP/RTSP/SRT URL, fragmented MP4 path, or HLS-fMP4 path.
  std::string target;
  OutputConfig zlm;
  // Positive limit for the delivery queue and, separately, the startup cache.
  // When either is full, the oldest packet is dropped for the newest one.
  // Ordered lifecycle notifications use no quota.
  std::size_t packet_queue_capacity = 384;
};

struct RtspPublishSinkConfig {
  // One RTSP path. All active publishers on the same bind address and port
  // share a listener; duplicate paths are rejected process-wide.
  std::string app;
  std::string stream;
  std::string bind_ip = "0.0.0.0";
  std::uint16_t port = 8554;
  MuxerConfig muxer;
  std::size_t packet_queue_capacity = 384;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_OUTPUT_CONFIG_H_

#ifndef MW_STREAMER_ZLM_CONFIG_H_
#define MW_STREAMER_ZLM_CONFIG_H_

#include <chrono>
#include <cstddef>
#include <string>

namespace mw::streamer {

struct PlayerConfig {
  std::chrono::milliseconds connect_timeout_ms{10000};
  std::chrono::milliseconds media_timeout_ms{5000};
  std::string local_bind_ip;
};

struct PusherConfig {
  std::chrono::milliseconds connect_timeout_ms{10000};
  std::string local_bind_ip;
};

struct MuxerConfig {
  // Zero disables ZLM paced sending.
  std::chrono::milliseconds paced_sender_interval_ms{0};
};

struct RecordingConfig {
  std::size_t file_buffer_size = 64 * 1024;
  std::chrono::milliseconds hls_segment_duration_ms{10000};
};

struct OutputConfig {
  PusherConfig pusher;
  MuxerConfig muxer;
  RecordingConfig recording;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ZLM_CONFIG_H_

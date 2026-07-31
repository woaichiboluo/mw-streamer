#ifndef MW_STREAMER_INCLUDE_MW_ZLM_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_ZLM_CONFIG_H_

#include <chrono>
#include <cstddef>
#include <string>

namespace mw::streamer::zlm {

// Process-wide ZLToolKit configuration consumed by mw::streamer::Init().
struct Config {
  // Zero lets ZLToolKit use std::thread::hardware_concurrency().
  std::size_t event_poller_threads = 0;
  std::size_t work_threads = 0;

  // Applies to both EventPollerPool and WorkThreadPool.
  bool enable_cpu_affinity = true;
};

struct PlayerConfig {
  std::chrono::milliseconds connect_timeout{10000};
  std::chrono::milliseconds media_timeout{5000};
  std::string local_bind_ip;
};

struct PusherConfig {
  std::chrono::milliseconds connect_timeout{10000};
  std::string local_bind_ip;
};

struct MuxerConfig {
  // Zero disables ZLM paced sending.
  std::chrono::milliseconds paced_sender_interval{0};
};

struct RecordingConfig {
  std::size_t file_buffer_size = 64 * 1024;
  std::chrono::milliseconds hls_segment_duration{2000};
};

struct OutputConfig {
  PusherConfig pusher;
  MuxerConfig muxer;
  RecordingConfig recording;
};

struct PipelineConfig {
  PlayerConfig player;
  OutputConfig output;
};

}  // namespace mw::streamer::zlm

#endif  // MW_STREAMER_INCLUDE_MW_ZLM_CONFIG_H_

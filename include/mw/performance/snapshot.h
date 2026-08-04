#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_SNAPSHOT_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_SNAPSHOT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mw::streamer::performance {

struct LatencySnapshot {
  std::uint64_t sample_count = 0;
  std::chrono::microseconds p50{0};
  std::chrono::microseconds p95{0};
  std::chrono::microseconds p99{0};
  std::chrono::microseconds max{0};
};

struct VideoStageSnapshot {
  std::uint64_t frames = 0;
  double frames_per_second = 0.0;
  LatencySnapshot latency;
};

struct AudioStageSnapshot {
  std::uint64_t samples = 0;
  double samples_per_second = 0.0;
  LatencySnapshot latency;
};

struct NetworkInputSnapshot {
  bool is_network = false;
  bool connected = false;
  std::uint64_t generation = 0;
  std::uint64_t reconnect_count = 0;
  std::uint64_t received_bytes = 0;
};

struct NetworkOutputSnapshot {
  std::string target;
  bool connected = false;
  std::uint64_t reconnect_count = 0;
  std::uint64_t sent_bytes = 0;
};

struct StreamingVideoSnapshot {
  VideoStageSnapshot decode;
  VideoStageSnapshot process;
  VideoStageSnapshot encode;
  std::uint64_t dropped_packets = 0;
  std::size_t queue_depth = 0;
};

struct StreamingAudioSnapshot {
  AudioStageSnapshot decode;
  AudioStageSnapshot process;
  AudioStageSnapshot encode;
  std::uint64_t dropped_packets = 0;
  std::size_t queue_depth = 0;
};

struct StreamingPipelineSnapshot {
  std::chrono::nanoseconds interval{0};
  NetworkInputSnapshot input;
  std::vector<NetworkOutputSnapshot> outputs;
  bool has_video = false;
  StreamingVideoSnapshot video;
  bool has_audio = false;
  StreamingAudioSnapshot audio;
  std::size_t output_queue_depth = 0;
};

struct RemuxPipelineSnapshot {
  std::chrono::nanoseconds interval{0};
  NetworkInputSnapshot input;
  std::vector<NetworkOutputSnapshot> outputs;
  std::uint64_t packets = 0;
  std::uint64_t bytes = 0;
  double bits_per_second = 0.0;
  std::size_t output_queue_depth = 0;
};

struct LocalFileVideoSnapshot {
  VideoStageSnapshot decode;
  VideoStageSnapshot process;
};

struct LocalFileAudioSnapshot {
  AudioStageSnapshot decode;
  AudioStageSnapshot process;
};

struct LocalFilePipelineSnapshot {
  std::chrono::nanoseconds interval{0};
  bool progress_available = false;
  std::chrono::microseconds processed_position{0};
  std::chrono::microseconds duration{0};
  double progress = 0.0;
  bool processing_speed_available = false;
  double processing_speed = 0.0;
  bool has_video = false;
  LocalFileVideoSnapshot video;
  bool has_audio = false;
  LocalFileAudioSnapshot audio;
};

}  // namespace mw::streamer::performance

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_SNAPSHOT_H_

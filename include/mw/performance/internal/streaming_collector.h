#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STREAMING_COLLECTOR_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STREAMING_COLLECTOR_H_

#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/snapshot.h"

namespace mw::streamer::performance::internal {

class StreamingCollector final {
 public:
  StreamingCollector() = default;

  StreamingCollector(const StreamingCollector&) = delete;
  StreamingCollector& operator=(const StreamingCollector&) = delete;

  TrackRecorder& audio() noexcept { return audio_; }
  TrackRecorder& video() noexcept { return video_; }

  void Reset();

  StreamingPipelineSnapshot Collect(bool has_audio, bool has_video,
                                    std::size_t audio_queue_depth,
                                    std::size_t video_queue_depth,
                                    std::size_t output_queue_depth,
                                    NetworkInputSnapshot input,
                                    std::vector<NetworkOutputSnapshot> outputs);

 private:
  std::mutex collection_mutex_;
  std::chrono::steady_clock::time_point last_collection_ =
      std::chrono::steady_clock::now();
  TrackRecorder audio_;
  TrackRecorder video_;
};

}  // namespace mw::streamer::performance::internal

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STREAMING_COLLECTOR_H_

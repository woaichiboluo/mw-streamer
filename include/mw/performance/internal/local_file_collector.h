#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_LOCAL_FILE_COLLECTOR_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_LOCAL_FILE_COLLECTOR_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/snapshot.h"

namespace mw::streamer::performance::internal {

class LocalFileCollector final {
 public:
  LocalFileCollector() = default;

  LocalFileCollector(const LocalFileCollector&) = delete;
  LocalFileCollector& operator=(const LocalFileCollector&) = delete;

  TrackRecorder& audio() noexcept { return audio_; }
  TrackRecorder& video() noexcept { return video_; }

  void Reset();
  void Configure(bool has_audio, bool has_video, bool duration_available,
                 std::chrono::microseconds duration);
  void RecordAudioPosition(std::chrono::microseconds position) noexcept;
  void RecordVideoPosition(std::chrono::microseconds position) noexcept;
  void MarkCompleted() noexcept;
  LocalFilePipelineSnapshot Collect();

 private:
  struct Position {
    bool available = false;
    std::int64_t microseconds = 0;
  };

  static void RecordPosition(std::atomic<std::int64_t>& destination,
                             std::atomic_bool& available,
                             std::chrono::microseconds position) noexcept;
  Position ProcessedPosition() const noexcept;

  std::mutex collection_mutex_;
  std::chrono::steady_clock::time_point last_collection_ =
      std::chrono::steady_clock::now();
  bool has_audio_ = false;
  bool has_video_ = false;
  bool duration_available_ = false;
  std::chrono::microseconds duration_{0};
  bool last_position_available_ = false;
  std::int64_t last_position_us_ = 0;
  TrackRecorder audio_;
  TrackRecorder video_;
  std::atomic<std::int64_t> audio_position_us_{0};
  std::atomic<std::int64_t> video_position_us_{0};
  std::atomic_bool audio_position_available_{false};
  std::atomic_bool video_position_available_{false};
  std::atomic_bool completed_{false};
};

}  // namespace mw::streamer::performance::internal

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_LOCAL_FILE_COLLECTOR_H_

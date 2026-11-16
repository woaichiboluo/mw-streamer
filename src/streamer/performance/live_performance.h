#ifndef MW_SRC_STREAMER_PERFORMANCE_LIVE_PERFORMANCE_H_
#define MW_SRC_STREAMER_PERFORMANCE_LIVE_PERFORMANCE_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mw::streamer::internal {

class LivePerformanceHistory final {
 public:
  struct Interval {
    double seconds = 0.;
    std::uint64_t input = 0;
    std::uint64_t output = 0;
    std::chrono::steady_clock::duration work{};
  };

  Interval Add(Interval interval) noexcept {
    intervals_[next_] = interval;
    next_ = (next_ + 1) % intervals_.size();
    Interval recent;
    for (const auto& sample : intervals_) {
      recent.seconds += sample.seconds;
      recent.input += sample.input;
      recent.output += sample.output;
      recent.work += sample.work;
    }
    return recent;
  }

 private:
  std::array<Interval, 5> intervals_{};
  std::size_t next_ = 0;
};

// The driver reports all counters at once. Video filtering and encoding
// contribute completed work from their respective workers.
class LivePerformance final {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kInterval = std::chrono::seconds(2);

  void Start(const void* instance, double target_fps) noexcept;
  void ReadVideo() noexcept;
  void FilteredVideo(Clock::duration elapsed, bool produced);
  void EncodedVideo();
  void VideoToEncoder() noexcept;
  void Resynced() noexcept;
  void BlockedVideo() noexcept;
  Clock::time_point Report(Clock::time_point now);

 private:
  struct Counters {
    std::uint64_t filter_in = 0;
    std::uint64_t filter_out = 0;
    std::uint64_t encoded = 0;
    Clock::duration filter_time{};
  };
  std::mutex mutex_;
  Counters pending_;
  LivePerformanceHistory recent_;
  Clock::time_point previous_{};
  const void* instance_ = nullptr;
  double target_fps_ = 0.;
  std::uint64_t resyncs_ = 0;
  std::uint64_t video_read_ = 0;
  std::uint64_t video_to_encoder_ = 0;
  std::uint64_t video_blocks_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_PERFORMANCE_LIVE_PERFORMANCE_H_

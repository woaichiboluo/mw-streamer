#ifndef MW_STREAMER_INTERNAL_VIDEO_SYNCHRONIZER_H_
#define MW_STREAMER_INTERNAL_VIDEO_SYNCHRONIZER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer::internal {

struct VideoSynchronizerConfig {
  std::size_t input_count = 1;
  std::size_t primary_input_index = 0;
  std::chrono::microseconds pts_tolerance{5'000};
  std::chrono::milliseconds maximum_wait{100};
  std::size_t queue_capacity = 8;
};

struct SynchronizedVideoFrames {
  std::vector<ffmpeg::Frame> frames;
  std::int64_t primary_pts_us = 0;
};

// VideoSynchronizer has a single-threaded owner. Push() and Pop() must be
// serialized by the caller.
class VideoSynchronizer final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit VideoSynchronizer(VideoSynchronizerConfig config = {});

  void Push(std::size_t input_index, ffmpeg::Frame frame,
            std::int64_t timeline_pts_us, TimePoint now = Clock::now());
  [[nodiscard]] std::optional<SynchronizedVideoFrames> Pop(
      TimePoint now = Clock::now());

  [[nodiscard]] std::size_t queued_frame_count(std::size_t input_index) const;

 private:
  struct QueuedFrame {
    ffmpeg::Frame frame;
    std::int64_t timeline_pts_us = 0;
    TimePoint arrival_time;
  };

  using Queue = std::deque<QueuedFrame>;

  [[nodiscard]] Queue::iterator FindClosest(Queue* queue, std::int64_t pts_us);
  void DiscardFramesTooOldFor(std::int64_t primary_pts_us);

  VideoSynchronizerConfig config_;
  std::vector<Queue> queues_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_VIDEO_SYNCHRONIZER_H_

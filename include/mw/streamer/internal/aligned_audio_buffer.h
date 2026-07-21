#ifndef MW_STREAMER_INTERNAL_ALIGNED_AUDIO_BUFFER_H_
#define MW_STREAMER_INTERNAL_ALIGNED_AUDIO_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "mw/streamer/internal/audio_converter.h"

namespace mw::streamer::internal {

class AlignedAudioBuffer {
 public:
  explicit AlignedAudioBuffer(
      std::size_t max_buffered_frames = kProcessorAudioSampleRate);

  void Push(NormalizedAudioFrame frame);
  void PushDroppingOldest(NormalizedAudioFrame frame);
  [[nodiscard]] NormalizedAudioFrame Read(int64_t start_pts,
                                          std::size_t frame_count);

  [[nodiscard]] std::size_t buffered_frame_count() const noexcept;
  [[nodiscard]] std::size_t remaining_capacity_frames() const noexcept;
  [[nodiscard]] std::optional<int64_t> accepted_until_pts() const noexcept;

 private:
  struct Segment {
    NormalizedAudioFrame frame;
    int64_t start_pts = 0;
    std::size_t consumed_frame_count = 0;
  };

  void PushImpl(NormalizedAudioFrame frame, bool drop_oldest);
  void DiscardOldest(std::size_t frame_count) noexcept;

  std::size_t max_buffered_frames_ = 0;
  std::size_t buffered_frame_count_ = 0;
  std::deque<Segment> segments_;
  int64_t accepted_until_pts_ = MW_TIMESTAMP_UNAVAILABLE;
  int64_t read_until_pts_ = MW_TIMESTAMP_UNAVAILABLE;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_ALIGNED_AUDIO_BUFFER_H_

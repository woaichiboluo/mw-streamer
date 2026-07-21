#ifndef MW_STREAMER_INTERNAL_AUDIO_FIFO_H_
#define MW_STREAMER_INTERNAL_AUDIO_FIFO_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "mw/streamer/internal/audio_converter.h"

namespace mw::streamer::internal {

class AudioFifo {
 public:
  explicit AudioFifo(
      std::size_t block_frame_count = kProcessorAudioBlockFrameCount,
      std::size_t max_buffered_frames = kProcessorAudioSampleRate);

  void Push(NormalizedAudioFrame frame);
  std::optional<NormalizedAudioFrame> Read();
  std::optional<NormalizedAudioFrame> Flush();

  [[nodiscard]] std::size_t buffered_frame_count() const noexcept;

 private:
  struct Segment {
    NormalizedAudioFrame frame;
    std::size_t consumed_frame_count = 0;
  };

  NormalizedAudioFrame ReadBlock(bool pad_with_silence);
  void SetOutputTimestamp(const Segment& segment,
                          NormalizedAudioFrame* output) const;

  std::size_t block_frame_count_ = 0;
  std::size_t max_buffered_frames_ = 0;
  std::size_t buffered_frame_count_ = 0;
  std::deque<Segment> segments_;
  int64_t expected_next_frame_pts_ = MW_TIMESTAMP_UNAVAILABLE;
  bool flushing_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_AUDIO_FIFO_H_

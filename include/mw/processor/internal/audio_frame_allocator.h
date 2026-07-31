#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_AUDIO_FRAME_ALLOCATOR_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_AUDIO_FRAME_ALLOCATOR_H_

extern "C" {
#include <libavutil/channel_layout.h>
}

#include "mw/ffmpeg/frame.h"

namespace mw::streamer::processor::internal {

class AudioFrameAllocator final {
 public:
  AudioFrameAllocator() = default;
  ~AudioFrameAllocator();

  AudioFrameAllocator(const AudioFrameAllocator&) = delete;
  AudioFrameAllocator& operator=(const AudioFrameAllocator&) = delete;
  AudioFrameAllocator(AudioFrameAllocator&&) = delete;
  AudioFrameAllocator& operator=(AudioFrameAllocator&&) = delete;

  // The first allocation captures the fixed output channel layout. Later
  // allocations may change nb_samples but must preserve that layout.
  ffmpeg::Frame Allocate(const ffmpeg::Frame& input);

 private:
  void PrepareOrValidate(const AVFrame& input);

  AVChannelLayout channel_layout_{};
  bool prepared_ = false;
};

}  // namespace mw::streamer::processor::internal

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_AUDIO_FRAME_ALLOCATOR_H_

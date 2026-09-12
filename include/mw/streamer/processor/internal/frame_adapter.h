#ifndef MW_STREAMER_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_
#define MW_STREAMER_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_

#include <array>

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/frame_view.h"
#include "mw/streamer/processor/processor.h"

namespace mw::streamer::internal {

using VideoFrameAdapter = VideoFrameViewAdapter;

class VideoBufferAdapter final {
 public:
  explicit VideoBufferAdapter(Frame& frame);

  VideoBufferAdapter(const VideoBufferAdapter&) = delete;
  VideoBufferAdapter& operator=(const VideoBufferAdapter&) = delete;
  VideoBufferAdapter(VideoBufferAdapter&&) = delete;
  VideoBufferAdapter& operator=(VideoBufferAdapter&&) = delete;

  const MwStreamerVideoBufferView& view() const noexcept;

 private:
  std::array<MwStreamerVideoPlaneView, 4> planes_{};
  MwStreamerVideoBufferView view_{};
};

using AudioFrameAdapter = AudioFrameViewAdapter;

class AudioBufferAdapter final {
 public:
  explicit AudioBufferAdapter(Frame& frame);

  const MwStreamerAudioBufferView& view() const noexcept;

 private:
  MwStreamerAudioBufferView view_{};
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_

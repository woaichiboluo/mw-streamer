#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_

#include <array>

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/frame_view.h"
#include "mw/processor/processor.h"

namespace mw::streamer::processor::internal {

using VideoFrameAdapter = ffmpeg::VideoFrameViewAdapter;

class VideoBufferAdapter final {
 public:
  explicit VideoBufferAdapter(ffmpeg::Frame& frame);

  VideoBufferAdapter(const VideoBufferAdapter&) = delete;
  VideoBufferAdapter& operator=(const VideoBufferAdapter&) = delete;
  VideoBufferAdapter(VideoBufferAdapter&&) = delete;
  VideoBufferAdapter& operator=(VideoBufferAdapter&&) = delete;

  const MwStreamerVideoBufferView& view() const noexcept;

 private:
  std::array<MwStreamerVideoPlaneView, 4> planes_{};
  MwStreamerVideoBufferView view_{};
};

using AudioFrameAdapter = ffmpeg::AudioFrameViewAdapter;

class AudioBufferAdapter final {
 public:
  explicit AudioBufferAdapter(ffmpeg::Frame& frame);

  const MwStreamerAudioBufferView& view() const noexcept;

 private:
  MwStreamerAudioBufferView view_{};
};

}  // namespace mw::streamer::processor::internal

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_FRAME_ADAPTER_H_

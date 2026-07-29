#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_FRAME_ADAPTER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_FRAME_ADAPTER_H_

#include <array>

#include "mw/ffmpeg/frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::processor {

// Adapters expose borrowed AVFrame storage through the Processor C ABI. They
// must remain alive for every use of the returned view and never retain the
// underlying frame.
class VideoFrameAdapter final {
 public:
  explicit VideoFrameAdapter(const ffmpeg::Frame& frame);

  VideoFrameAdapter(const VideoFrameAdapter&) = delete;
  VideoFrameAdapter& operator=(const VideoFrameAdapter&) = delete;
  VideoFrameAdapter(VideoFrameAdapter&&) = delete;
  VideoFrameAdapter& operator=(VideoFrameAdapter&&) = delete;

  const MwStreamerVideoFrameView& view() const noexcept;

 private:
  std::array<MwStreamerVideoPlaneView, 4> planes_{};
  MwStreamerVideoFrameView view_{};
};

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

class AudioFrameAdapter final {
 public:
  explicit AudioFrameAdapter(const ffmpeg::Frame& frame);

  const MwStreamerAudioFrameView& view() const noexcept;

 private:
  MwStreamerAudioFrameView view_{};
};

class AudioBufferAdapter final {
 public:
  explicit AudioBufferAdapter(ffmpeg::Frame& frame);

  const MwStreamerAudioBufferView& view() const noexcept;

 private:
  MwStreamerAudioBufferView view_{};
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_FRAME_ADAPTER_H_

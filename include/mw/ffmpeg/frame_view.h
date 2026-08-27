#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_VIEW_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_VIEW_H_

#include <array>

#include "mw/ffmpeg/frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::ffmpeg {

// Exposes a borrowed C view of a video Frame without copying its buffers. The
// source Frame must remain alive and unchanged, and this adapter must remain
// alive because the returned view references its plane descriptors.
class VideoFrameViewAdapter final {
 public:
  explicit VideoFrameViewAdapter(const Frame& frame);

  VideoFrameViewAdapter(const VideoFrameViewAdapter&) = delete;
  VideoFrameViewAdapter& operator=(const VideoFrameViewAdapter&) = delete;
  VideoFrameViewAdapter(VideoFrameViewAdapter&&) = delete;
  VideoFrameViewAdapter& operator=(VideoFrameViewAdapter&&) = delete;

  const MwStreamerVideoFrameView& view() const noexcept;

 private:
  std::array<MwStreamerVideoPlaneView, 4> planes_{};
  MwStreamerVideoFrameView view_{};
};

// Exposes a borrowed C view of an audio Frame without copying its samples. The
// source Frame must remain alive and unchanged, and this adapter must remain
// alive for every use of the returned view.
class AudioFrameViewAdapter final {
 public:
  explicit AudioFrameViewAdapter(const Frame& frame);

  const MwStreamerAudioFrameView& view() const noexcept;

 private:
  MwStreamerAudioFrameView view_{};
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_VIEW_H_

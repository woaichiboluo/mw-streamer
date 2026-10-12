#ifndef MW_STREAMER_MEDIA_FRAME_H_
#define MW_STREAMER_MEDIA_FRAME_H_

#include <optional>

#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer {

// One channel frame, carrying its picture and corresponding audio samples.
// A single-track channel leaves the other member empty. Copying retains the
// underlying CPU/CUDA buffers without copying their media payload.
struct MediaFrame {
  std::optional<ffmpeg::Frame> video;
  std::optional<ffmpeg::Frame> audio;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_MEDIA_FRAME_H_

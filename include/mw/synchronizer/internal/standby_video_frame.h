#ifndef MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_
#define MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_

#include <string>

#include "mw/ffmpeg/frame.h"

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::synchronizer::internal {

// Builds one immutable standby image in the exact software or hardware format
// used by the video encoder. Each Frame call only refs the cached buffers.
class StandbyVideoFrame final {
 public:
  explicit StandbyVideoFrame(std::string image_path);

  void Prepare(const ffmpeg::Frame& prototype,
               const ffmpeg::HardwareContext* hardware_context);
  bool prepared() const noexcept;
  ffmpeg::Frame Ref() const;

 private:
  std::string image_path_;
  ffmpeg::Frame frame_;
  bool prepared_ = false;
};

}  // namespace mw::streamer::synchronizer::internal

#endif  // MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_

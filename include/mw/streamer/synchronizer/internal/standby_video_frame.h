#ifndef MW_STREAMER_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_
#define MW_STREAMER_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_

#include <string>

#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer {
class HardwareContext;
}

namespace mw::streamer::internal {

// Builds one immutable standby image in the exact software or hardware format
// used by the video encoder. Each Frame call only refs the cached buffers.
class StandbyVideoFrame final {
 public:
  explicit StandbyVideoFrame(std::string image_path);

  void Prepare(const Frame& prototype,
               const HardwareContext* hardware_context);
  bool prepared() const noexcept;
  Frame Ref() const;

 private:
  std::string image_path_;
  Frame frame_;
  bool prepared_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_SYNCHRONIZER_INTERNAL_STANDBY_VIDEO_FRAME_H_

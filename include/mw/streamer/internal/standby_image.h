#ifndef MW_STREAMER_INTERNAL_STANDBY_IMAGE_H_
#define MW_STREAMER_INTERNAL_STANDBY_IMAGE_H_

#include <optional>
#include <string_view>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/gpu_nv12_frame_pool.h"

namespace mw::streamer::internal {

[[nodiscard]] ffmpeg::Frame DecodeStandbyImageToNv12(std::string_view path,
                                                     int output_width,
                                                     int output_height,
                                                     AVColorRange color_range,
                                                     AVColorSpace color_space);

class StandbyImage final {
 public:
  void Load(std::string_view path, GpuNv12FramePool* frame_pool,
            AVColorRange color_range, AVColorSpace color_space);
  [[nodiscard]] ffmpeg::Frame Ref() const;

  [[nodiscard]] bool is_loaded() const noexcept;

 private:
  std::optional<ffmpeg::Frame> frame_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_STANDBY_IMAGE_H_

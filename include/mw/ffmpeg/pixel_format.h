#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace mw::streamer {

bool IsHardwarePixelFormat(AVPixelFormat format) noexcept;

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_

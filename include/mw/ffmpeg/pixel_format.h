#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace mw::streamer::ffmpeg {

bool IsHardwarePixelFormat(AVPixelFormat format) noexcept;

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_PIXEL_FORMAT_H_

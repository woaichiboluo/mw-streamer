#include "mw/streamer/ffmpeg/pixel_format.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace mw::streamer {

bool IsHardwarePixelFormat(AVPixelFormat format) noexcept {
  const auto* descriptor = av_pix_fmt_desc_get(format);
  return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

}  // namespace mw::streamer

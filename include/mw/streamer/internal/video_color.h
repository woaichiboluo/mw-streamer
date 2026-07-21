#ifndef MW_STREAMER_INTERNAL_VIDEO_COLOR_H_
#define MW_STREAMER_INTERNAL_VIDEO_COLOR_H_

extern "C" {
#include <libavutil/pixfmt.h>
}

#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

[[nodiscard]] MwVideoColorRange ToMwColorRange(AVColorRange range) noexcept;
[[nodiscard]] MwVideoColorSpace ToMwColorSpace(AVColorSpace space) noexcept;
[[nodiscard]] AVColorRange ToAvColorRange(MwVideoColorRange range) noexcept;
[[nodiscard]] AVColorSpace ToAvColorSpace(MwVideoColorSpace space) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_VIDEO_COLOR_H_

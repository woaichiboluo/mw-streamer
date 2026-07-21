#include "mw/streamer/internal/video_color.h"

namespace mw::streamer::internal {

MwVideoColorRange ToMwColorRange(AVColorRange range) noexcept {
  switch (range) {
    case AVCOL_RANGE_MPEG:
      return MW_VIDEO_COLOR_RANGE_LIMITED;
    case AVCOL_RANGE_JPEG:
      return MW_VIDEO_COLOR_RANGE_FULL;
    default:
      return MW_VIDEO_COLOR_RANGE_UNSPECIFIED;
  }
}

MwVideoColorSpace ToMwColorSpace(AVColorSpace space) noexcept {
  switch (space) {
    case AVCOL_SPC_BT709:
      return MW_VIDEO_COLOR_SPACE_BT709;
    case AVCOL_SPC_FCC:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      return MW_VIDEO_COLOR_SPACE_BT601;
    case AVCOL_SPC_BT2020_NCL:
      return MW_VIDEO_COLOR_SPACE_BT2020_NCL;
    case AVCOL_SPC_BT2020_CL:
      return MW_VIDEO_COLOR_SPACE_BT2020_CL;
    default:
      return MW_VIDEO_COLOR_SPACE_UNSPECIFIED;
  }
}

AVColorRange ToAvColorRange(MwVideoColorRange range) noexcept {
  if (range == MW_VIDEO_COLOR_RANGE_LIMITED) {
    return AVCOL_RANGE_MPEG;
  }
  if (range == MW_VIDEO_COLOR_RANGE_FULL) {
    return AVCOL_RANGE_JPEG;
  }
  return AVCOL_RANGE_UNSPECIFIED;
}

AVColorSpace ToAvColorSpace(MwVideoColorSpace space) noexcept {
  if (space == MW_VIDEO_COLOR_SPACE_BT601) {
    return AVCOL_SPC_SMPTE170M;
  }
  if (space == MW_VIDEO_COLOR_SPACE_BT709) {
    return AVCOL_SPC_BT709;
  }
  if (space == MW_VIDEO_COLOR_SPACE_BT2020_NCL) {
    return AVCOL_SPC_BT2020_NCL;
  }
  if (space == MW_VIDEO_COLOR_SPACE_BT2020_CL) {
    return AVCOL_SPC_BT2020_CL;
  }
  return AVCOL_SPC_UNSPECIFIED;
}

}  // namespace mw::streamer::internal

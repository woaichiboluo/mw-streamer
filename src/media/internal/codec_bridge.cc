#include "mw/media/internal/codec_bridge.h"

#include <array>
#include <utility>

namespace mw::streamer::media::internal {
namespace {

constexpr std::array kCodecMappings = {
    std::pair{kMwStreamerCodecH264, AV_CODEC_ID_H264},
    std::pair{kMwStreamerCodecH265, AV_CODEC_ID_HEVC},
    std::pair{kMwStreamerCodecAv1, AV_CODEC_ID_AV1},
    std::pair{kMwStreamerCodecAac, AV_CODEC_ID_AAC},
    std::pair{kMwStreamerCodecG711A, AV_CODEC_ID_PCM_ALAW},
    std::pair{kMwStreamerCodecG711U, AV_CODEC_ID_PCM_MULAW},
    std::pair{kMwStreamerCodecOpus, AV_CODEC_ID_OPUS},
    std::pair{kMwStreamerCodecMjpeg, AV_CODEC_ID_MJPEG},
    std::pair{kMwStreamerCodecVp8, AV_CODEC_ID_VP8},
    std::pair{kMwStreamerCodecVp9, AV_CODEC_ID_VP9},
};

}  // namespace

MwStreamerCodec ToMwStreamerCodec(AVCodecID codec_id) noexcept {
  for (const auto& [mw_codec, av_codec] : kCodecMappings) {
    if (av_codec == codec_id) {
      return mw_codec;
    }
  }
  return kMwStreamerCodecUnknown;
}

AVCodecID ToAvCodecId(MwStreamerCodec codec) noexcept {
  for (const auto& [mw_codec, av_codec] : kCodecMappings) {
    if (mw_codec == codec) {
      return av_codec;
    }
  }
  return AV_CODEC_ID_NONE;
}

}  // namespace mw::streamer::media::internal

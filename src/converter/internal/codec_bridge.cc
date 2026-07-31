#include "mw/converter/internal/codec_bridge.h"

#include <array>
#include <utility>

namespace mw::streamer::converter::internal {
namespace {

constexpr std::array kCodecMappings{
    std::pair{AV_CODEC_ID_H264, mediakit::CodecH264},
    std::pair{AV_CODEC_ID_HEVC, mediakit::CodecH265},
    std::pair{AV_CODEC_ID_AAC, mediakit::CodecAAC},
    std::pair{AV_CODEC_ID_PCM_ALAW, mediakit::CodecG711A},
    std::pair{AV_CODEC_ID_PCM_MULAW, mediakit::CodecG711U},
    std::pair{AV_CODEC_ID_OPUS, mediakit::CodecOpus},
    std::pair{AV_CODEC_ID_MJPEG, mediakit::CodecJPEG},
    std::pair{AV_CODEC_ID_VP8, mediakit::CodecVP8},
    std::pair{AV_CODEC_ID_VP9, mediakit::CodecVP9},
};

}  // namespace

mediakit::CodecId ToZlmCodecId(AVCodecID codec_id) noexcept {
  for (const auto& [ffmpeg_codec, zlm_codec] : kCodecMappings) {
    if (ffmpeg_codec == codec_id) {
      return zlm_codec;
    }
  }
  return mediakit::CodecInvalid;
}

AVCodecID ToFfmpegCodecId(mediakit::CodecId codec_id) noexcept {
  for (const auto& [ffmpeg_codec, zlm_codec] : kCodecMappings) {
    if (zlm_codec == codec_id) {
      return ffmpeg_codec;
    }
  }
  return AV_CODEC_ID_NONE;
}

}  // namespace mw::streamer::converter::internal

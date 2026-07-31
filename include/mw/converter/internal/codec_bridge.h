#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_CODEC_BRIDGE_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_CODEC_BRIDGE_H_

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "Extension/Frame.h"

namespace mw::streamer::converter::internal {

mediakit::CodecId ToZlmCodecId(AVCodecID codec_id) noexcept;
AVCodecID ToFfmpegCodecId(mediakit::CodecId codec_id) noexcept;

}  // namespace mw::streamer::converter::internal

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_INTERNAL_CODEC_BRIDGE_H_

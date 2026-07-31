#ifndef MW_STREAMER_INCLUDE_MW_MEDIA_INTERNAL_CODEC_BRIDGE_H_
#define MW_STREAMER_INCLUDE_MW_MEDIA_INTERNAL_CODEC_BRIDGE_H_

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "mw/media/types.h"

namespace mw::streamer::media::internal {

MwStreamerCodec ToMwStreamerCodec(AVCodecID codec_id) noexcept;
AVCodecID ToAvCodecId(MwStreamerCodec codec) noexcept;

}  // namespace mw::streamer::media::internal

#endif  // MW_STREAMER_INCLUDE_MW_MEDIA_INTERNAL_CODEC_BRIDGE_H_

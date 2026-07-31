#ifndef MW_STREAMER_INCLUDE_MW_DECODER_INTERNAL_CODEC_FINDER_H_
#define MW_STREAMER_INCLUDE_MW_DECODER_INTERNAL_CODEC_FINDER_H_

#include <string>

extern "C" {
#include <libavcodec/codec.h>
}

#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::decoder::internal {

const AVCodec* FindDecoder(const ffmpeg::StreamInfo& stream_info,
                           const std::string& decoder_name,
                           AVMediaType media_type);

}  // namespace mw::streamer::decoder::internal

#endif  // MW_STREAMER_INCLUDE_MW_DECODER_INTERNAL_CODEC_FINDER_H_

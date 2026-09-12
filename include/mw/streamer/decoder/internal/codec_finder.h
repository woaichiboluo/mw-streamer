#ifndef MW_STREAMER_DECODER_INTERNAL_CODEC_FINDER_H_
#define MW_STREAMER_DECODER_INTERNAL_CODEC_FINDER_H_

#include <string>

extern "C" {
#include <libavcodec/codec.h>
}

#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

const AVCodec* FindDecoder(const StreamInfo& stream_info,
                           const std::string& decoder_name,
                           AVMediaType media_type);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_DECODER_INTERNAL_CODEC_FINDER_H_

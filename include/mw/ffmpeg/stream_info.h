#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_STREAM_INFO_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_STREAM_INFO_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include "mw/ffmpeg/codec_parameters.h"

namespace mw::streamer::ffmpeg {

struct StreamInfo {
  int stream_index = -1;
  CodecParameters codec_parameters;
  AVRational time_base{0, 1};

  static StreamInfo FromCodecContext(const AVCodecContext& context,
                                     int stream_index);
  void Validate() const;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_STREAM_INFO_H_

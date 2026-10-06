#ifndef MW_STREAMER_FFMPEG_STREAM_INFO_H_
#define MW_STREAMER_FFMPEG_STREAM_INFO_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include "mw/export.h"
#include "mw/streamer/ffmpeg/codec_parameters.h"

namespace mw::streamer::ffmpeg {

struct MW_STREAMER_API StreamInfo {
  int stream_index = -1;
  CodecParameters codec_parameters;
  AVRational time_base{0, 1};

  // Compare track identity, time base and audio/video format properties.
  // Other codec metadata and extradata are ignored.
  bool operator==(const StreamInfo& other) const noexcept;

  static StreamInfo FromCodecContext(const AVCodecContext& context,
                                     int stream_index);
  void Validate() const;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_STREAM_INFO_H_

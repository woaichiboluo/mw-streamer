#ifndef MW_STREAMER_FFMPEG_STREAM_INFO_H_
#define MW_STREAMER_FFMPEG_STREAM_INFO_H_

#include <map>
#include <string>

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
  // Original input stream metadata, used by downstream track selection and
  // filter construction. Encoder-generated descriptions keep these empty.
  int disposition = 0;
  std::map<std::string, std::string> metadata{};
  // Demuxer-derived video hints, independent of the original codec parameters.
  AVRational frame_rate{0, 1};
  AVRational sample_aspect_ratio{0, 1};

  // Compare track identity, time base and audio/video format properties.
  // Other codec metadata and extradata are ignored.
  bool operator==(const StreamInfo& other) const noexcept;

  static StreamInfo FromCodecContext(const AVCodecContext& context,
                                     int stream_index);
  void Validate() const;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_STREAM_INFO_H_

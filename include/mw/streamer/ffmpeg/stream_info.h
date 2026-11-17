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
  // Average input video rate (AVStream::avg_frame_rate), matching FFmpeg's
  // displayed fps. A zero numerator or denominator means unknown; this does
  // not guarantee constant frame intervals. Used for automatic output fps.
  AVRational frame_rate{0, 1};
  AVRational sample_aspect_ratio{0, 1};
  // Base rate (AVStream::r_frame_rate), displayed by FFmpeg as tbr. This can
  // exceed the average rate and must not substitute for an unknown average.
  AVRational base_frame_rate{0, 1};
  // av_guess_frame_rate() hint for decoder setup and missing-duration
  // estimates. Kept separate so average-fps selection does not alter decoding.
  AVRational decoder_frame_rate{0, 1};

  // Compare track identity, time base and audio/video format properties.
  // Other codec metadata and extradata are ignored.
  bool operator==(const StreamInfo& other) const noexcept;

  static StreamInfo FromCodecContext(const AVCodecContext& context,
                                     int stream_index);
  void Validate() const;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_STREAM_INFO_H_

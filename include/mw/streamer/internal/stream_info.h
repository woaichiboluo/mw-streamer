#ifndef MW_STREAMER_INTERNAL_STREAM_INFO_H_
#define MW_STREAMER_INTERNAL_STREAM_INFO_H_

extern "C" {
#include <libavutil/rational.h>
}

#include "mw/streamer/ffmpeg/codec_parameters.h"

namespace mw::streamer::internal {

struct StreamInfo {
  StreamInfo() = default;
  ~StreamInfo() noexcept = default;

  StreamInfo(const StreamInfo&) = delete;
  StreamInfo& operator=(const StreamInfo&) = delete;
  StreamInfo(StreamInfo&&) noexcept = default;
  StreamInfo& operator=(StreamInfo&&) noexcept = default;

  int index = -1;
  AVRational time_base{0, 1};
  AVRational frame_rate{0, 1};
  ffmpeg::CodecParameters codec_parameters;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_STREAM_INFO_H_

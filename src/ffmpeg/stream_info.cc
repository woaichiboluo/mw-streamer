#include "mw/ffmpeg/stream_info.h"

#include <stdexcept>

namespace mw::streamer::ffmpeg {

void StreamInfo::Validate() const {
  const auto* parameters = codec_parameters.get();
  if (stream_index < 0 || !parameters || time_base.num <= 0 ||
      time_base.den <= 0 || parameters->codec_id == AV_CODEC_ID_NONE ||
      (parameters->codec_type != AVMEDIA_TYPE_AUDIO &&
       parameters->codec_type != AVMEDIA_TYPE_VIDEO) ||
      parameters->extradata_size < 0 ||
      (parameters->extradata_size > 0 && !parameters->extradata)) {
    throw std::invalid_argument("FFmpeg StreamInfo参数无效");
  }
}

}  // namespace mw::streamer::ffmpeg

#include "mw/ffmpeg/stream_info.h"

#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {

StreamInfo StreamInfo::FromCodecContext(const AVCodecContext& context,
                                        int stream_index) {
  CodecParameters parameters;
  ThrowIfError(avcodec_parameters_from_context(parameters.get(), &context),
               "从AVCodecContext导出CodecParameters");
  StreamInfo stream_info{
      stream_index,
      std::move(parameters),
      context.time_base,
  };
  stream_info.Validate();
  return stream_info;
}

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

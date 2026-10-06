#include "mw/streamer/ffmpeg/stream_info.h"

#include <stdexcept>
#include <utility>

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

bool SameRatio(AVRational left, AVRational right) noexcept {
  return (left.num == 0 && right.num == 0) || av_cmp_q(left, right) == 0;
}

}  // namespace

bool StreamInfo::operator==(const StreamInfo& other) const noexcept {
  if (stream_index != other.stream_index ||
      !SameRatio(time_base, other.time_base)) {
    return false;
  }
  const auto* left = codec_parameters.get();
  const auto* right = other.codec_parameters.get();
  if (!left || !right) {
    return left == right;
  }
  if (left->codec_type != right->codec_type ||
      left->codec_id != right->codec_id) {
    return false;
  }
  switch (left->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      return left->format == right->format && left->width == right->width &&
             left->height == right->height &&
             SameRatio(left->framerate, right->framerate);
    case AVMEDIA_TYPE_AUDIO:
      return left->format == right->format &&
             left->sample_rate == right->sample_rate &&
             av_channel_layout_compare(&left->ch_layout, &right->ch_layout) ==
                 0;
    default:
      return true;
  }
}

StreamInfo StreamInfo::FromCodecContext(const AVCodecContext& context,
                                        int stream_index) {
  CodecParameters parameters;
  FfmpegException::throwIfError(
      avcodec_parameters_from_context(parameters.get(), &context),
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

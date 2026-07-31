#include "mw/decoder/internal/codec_finder.h"

#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <fmt/format.h>

namespace mw::streamer::decoder::internal {
namespace {

const char* MediaTypeName(AVMediaType media_type) {
  const char* name = av_get_media_type_string(media_type);
  return name ? name : "unknown";
}

}  // namespace

const AVCodec* FindDecoder(const ffmpeg::StreamInfo& stream_info,
                           const std::string& decoder_name,
                           AVMediaType media_type) {
  stream_info.Validate();
  const auto* parameters = stream_info.codec_parameters.get();
  if (parameters->codec_type != media_type) {
    throw std::invalid_argument(fmt::format(
        "解码器只接受{}流: input_media_type={}", MediaTypeName(media_type),
        MediaTypeName(parameters->codec_type)));
  }

  const auto* codec = decoder_name.empty()
                          ? avcodec_find_decoder(parameters->codec_id)
                          : avcodec_find_decoder_by_name(decoder_name.c_str());
  if (!codec) {
    throw std::invalid_argument(
        fmt::format("找不到{}解码器: codec_id={}, decoder_name={}",
                    MediaTypeName(media_type),
                    static_cast<int>(parameters->codec_id), decoder_name));
  }
  if (codec->type != media_type || codec->id != parameters->codec_id) {
    throw std::invalid_argument(fmt::format(
        "{}解码器与输入编码不匹配: decoder_name={}, decoder_codec_id={}, "
        "input_codec_id={}",
        MediaTypeName(media_type), codec->name, static_cast<int>(codec->id),
        static_cast<int>(parameters->codec_id)));
  }
  return codec;
}

}  // namespace mw::streamer::decoder::internal

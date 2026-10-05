#ifndef MW_STREAMER_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_
#define MW_STREAMER_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_

#include <memory>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/rational.h>
}

#include "Extension/Track.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

constexpr AVRational kZlmTimeBase{1, 1000};

mediakit::CodecId ToZlmCodecId(AVCodecID codec_id) noexcept;
AVCodecID ToFfmpegCodecId(mediakit::CodecId codec_id) noexcept;

class ZlmCodecParametersConverter {
 public:
  using Ptr = std::shared_ptr<ZlmCodecParametersConverter>;

  explicit ZlmCodecParametersConverter(const mediakit::Track::Ptr& track);

  const ffmpeg::CodecParameters& codec_parameters() const;
  AVRational time_base() const;

 private:
  ffmpeg::CodecParameters codec_parameters_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_

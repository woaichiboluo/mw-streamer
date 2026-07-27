#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_

#include <memory>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/rational.h>
}

#include "Extension/Track.h"

namespace mw::streamer::converter {

class ZlmCodecParametersConverter {
 public:
  using Ptr = std::shared_ptr<ZlmCodecParametersConverter>;
  using CodecParametersPtr = std::shared_ptr<AVCodecParameters>;

  explicit ZlmCodecParametersConverter(const mediakit::Track::Ptr& track);

  const CodecParametersPtr& codec_parameters() const;
  AVRational time_base() const;

 private:
  CodecParametersPtr codec_parameters_;
};

}  // namespace mw::streamer::converter

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_CODEC_PARAMETERS_CONVERTER_H_

#pragma once

#include <memory>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/rational.h>
}

#include "Extension/Track.h"

namespace mw::convter {

class ZlmCodecParametersConvter {
 public:
  using Ptr = std::shared_ptr<ZlmCodecParametersConvter>;
  using CodecParametersPtr = std::shared_ptr<AVCodecParameters>;

  explicit ZlmCodecParametersConvter(const mediakit::Track::Ptr& track);

  const CodecParametersPtr& getCodecParameters() const;
  AVRational getTimeBase() const;

 private:
  CodecParametersPtr codec_parameters_;
};

}  // namespace mw::convter

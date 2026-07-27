#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

#include "Extension/Frame.h"

namespace mw::streamer::converter {

class AvPacketToZlmFrameConverter final {
 public:
  using Ptr = std::shared_ptr<AvPacketToZlmFrameConverter>;

  AvPacketToZlmFrameConverter(const AVCodecParameters& codec_parameters,
                              AVRational time_base, int stream_index);

  // The returned frame owns or references its payload independently of packet.
  // H264/H265 packets must use Annex-B framing.
  mediakit::Frame::Ptr Convert(const AVPacket* packet) const;

 private:
  mediakit::CodecId codec_id_;
  AVRational time_base_;
  int stream_index_;
  std::string aac_config_;
};

}  // namespace mw::streamer::converter

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_

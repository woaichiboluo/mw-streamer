#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/rational.h>
}

#include "Extension/Frame.h"
#include "mw/ffmpeg/codec_parameters.h"
#include "mw/ffmpeg/packet.h"

namespace mw::streamer::converter {

class AvPacketToZlmFrameConverter final {
 public:
  using Ptr = std::shared_ptr<AvPacketToZlmFrameConverter>;

  AvPacketToZlmFrameConverter(const ffmpeg::CodecParameters& codec_parameters,
                              AVRational time_base, int stream_index);

  // Takes ownership of packet's reference. The returned frames keep the
  // payload alive. H264/H265 packets must use Annex-B framing and produce one
  // ZLM Frame per NAL unit.
  std::vector<mediakit::Frame::Ptr> Convert(ffmpeg::Packet packet) const;

 private:
  mediakit::CodecId codec_id_;
  AVRational time_base_;
  int stream_index_;
  std::string aac_config_;
};

}  // namespace mw::streamer::converter

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_AV_PACKET_TO_ZLM_FRAME_CONVERTER_H_

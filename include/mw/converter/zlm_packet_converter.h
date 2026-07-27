#ifndef MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_PACKET_CONVERTER_H_
#define MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_PACKET_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

#include "Extension/Track.h"

namespace mw::streamer::converter {

class ZlmPacketConverter {
 public:
  using Ptr = std::shared_ptr<ZlmPacketConverter>;
  using OnPacket = std::function<bool(const AVPacket* packet)>;

  ZlmPacketConverter(const mediakit::Track::Ptr& track, int stream_index);

  void SetOnPacket(OnPacket callback);
  bool InputFrame(const mediakit::Frame::Ptr& frame);
  bool Flush();
  void Reset();

 private:
  bool EmitPacket(const char* data, size_t size, std::uint64_t dts,
                  std::uint64_t pts, bool key_frame);

  mediakit::CodecId codec_id_;
  int stream_index_;
  bool initial_config_sent_ = false;
  bool last_output_result_ = true;
  std::vector<mediakit::Frame::Ptr> initial_config_frames_;
  OnPacket on_packet_;
  mediakit::FrameMerger merger_{mediakit::FrameMerger::h264_prefix};
};

}  // namespace mw::streamer::converter

#endif  // MW_STREAMER_INCLUDE_MW_CONVERTER_ZLM_PACKET_CONVERTER_H_

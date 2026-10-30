#ifndef MW_SRC_STREAMER_REMUXER_PACKET_CONVERTER_H_
#define MW_SRC_STREAMER_REMUXER_PACKET_CONVERTER_H_

#include <cstdint>
#include <vector>

#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

// Prepared tracks and owned packets for one serialized muxer session.
// Frames borrow retained AVPacket buffers; length-prefixed video is converted
// once to Annex B before the muxer shares it between outputs.
class PacketConverter final {
 public:
  // Configuration may be supplied globally or by video parameter sets in
  // Annex B packets and AAC configuration in ADTS packets.
  explicit PacketConverter(const std::vector<ffmpeg::StreamInfo>& streams);
  const std::vector<mediakit::Track::Ptr>& tracks() const noexcept;
  // Invalid video NAL headers are filtered without modifying the input;
  // returns nullptr if no valid NAL remains. Structural truncation is fatal.
  mediakit::Frame::Ptr Convert(const ffmpeg::Packet& packet,
                               std::uint64_t dts_ms, std::uint64_t pts_ms);

 private:
  struct Stream {
    int index;
    AVRational time_base;
    mediakit::CodecId codec;
    int nal_length_size;
  };
  std::vector<Stream> streams_;
  std::vector<mediakit::Track::Ptr> tracks_;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_REMUXER_PACKET_CONVERTER_H_

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
}

#include "Extension/Track.h"

namespace mw::convter {

class ZlmPacketConvter {
 public:
  using Ptr = std::shared_ptr<ZlmPacketConvter>;
  using OnPacket = std::function<bool(const AVPacket* packet)>;

  ZlmPacketConvter(const mediakit::Track::Ptr& track, int stream_index);

  void setOnPacket(OnPacket callback);
  bool inputFrame(const mediakit::Frame::Ptr& frame);
  bool flush();
  void reset();

 private:
  bool emitPacket(const char* data, size_t size, std::uint64_t dts,
                  std::uint64_t pts, bool key_frame);

  mediakit::CodecId codec_id_;
  int stream_index_;
  bool initial_config_sent_ = false;
  bool last_output_result_ = true;
  std::vector<mediakit::Frame::Ptr> initial_config_frames_;
  OnPacket on_packet_;
  mediakit::FrameMerger merger_{mediakit::FrameMerger::h264_prefix};
};

}  // namespace mw::convter

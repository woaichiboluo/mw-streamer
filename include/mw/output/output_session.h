#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

#include <memory>
#include <string>
#include <vector>

#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::output {

struct OutputConfig {
  std::vector<ffmpeg::StreamInfo> streams;
  // RTMP, RTSP, and SRT URLs are network targets. Paths ending in .mp4 and
  // .m3u8 are fragmented MP4 and HLS-fMP4 recording targets respectively.
  std::vector<std::string> targets;
  zlm::OutputConfig zlm;
};

class OutputSession final {
 public:
  using Ptr = std::shared_ptr<OutputSession>;

  explicit OutputSession(
      OutputConfig config,
      std::shared_ptr<toolkit::EventPoller> poller = nullptr);
  ~OutputSession();

  OutputSession(const OutputSession&) = delete;
  OutputSession& operator=(const OutputSession&) = delete;

  // Open validates configuration synchronously. Video output discards packets
  // before the first key frame; protocol publishing starts after every Track
  // is ready from that decodable boundary.
  void Open();

  // Write borrows packet for the call and retains one reference for queued
  // processing. Runtime target failures are logged and do not propagate to the
  // input, cache, or other output targets.
  void Write(const ffmpeg::Packet& packet);

  // Close stops all pushers and finalizes every recording target. The session
  // cannot be opened again after Close.
  void Close() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

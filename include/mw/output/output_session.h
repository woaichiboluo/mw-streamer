#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

#include <cstdint>
#include <functional>
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

struct NetworkTraffic {
  std::string target;
  bool connected = false;
  std::uint64_t reconnect_count = 0;
  std::uint64_t sent_bytes = 0;
};

class OutputSession final {
 public:
  using Ptr = std::shared_ptr<OutputSession>;
  using OnAllTargetsUnavailable = std::function<void()>;

  explicit OutputSession(
      OutputConfig config,
      std::shared_ptr<toolkit::EventPoller> poller = nullptr);
  ~OutputSession();

  OutputSession(const OutputSession&) = delete;
  OutputSession& operator=(const OutputSession&) = delete;

  // The callback runs on the session poller after every configured target has
  // become permanently unavailable. Retrying network targets remain available.
  // It must be set before Open and is never emitted by an explicit Close.
  void SetOnAllTargetsUnavailable(OnAllTargetsUnavailable callback);

  // Open validates configuration synchronously. Video output discards packets
  // before the first key frame; protocol publishing starts after every Track
  // is ready from that decodable boundary.
  void Open();

  // Write borrows packet for the call and retains one reference for queued
  // processing. Runtime target failures are logged and do not propagate to the
  // input, cache, or other output targets.
  void Write(const ffmpeg::Packet& packet);

  std::vector<NetworkTraffic> GetNetworkTraffic() const;

  // Close stops all pushers and finalizes every recording target. The session
  // cannot be opened again after Close.
  void Close() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

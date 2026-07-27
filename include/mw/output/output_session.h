#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
}

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::output {

struct OutputStreamInfo {
  int stream_index = -1;
  std::shared_ptr<AVCodecParameters> codec_parameters;
  AVRational time_base{0, 1};
};

struct OutputConfig {
  std::vector<OutputStreamInfo> streams;
  // RTMP, RTSP, and SRT URLs are network targets. Paths ending in .mp4 and
  // .m3u8 are fragmented MP4 and HLS-fMP4 recording targets respectively.
  std::vector<std::string> targets;
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

  // Open validates configuration synchronously. Protocol publishing starts
  // after ZLM has received enough frames to make every Track ready.
  void Open();

  // Write takes no ownership of packet. Calls made off the owner poller clone
  // the packet before queuing it. Runtime target failures are logged and do not
  // propagate to the input, cache, or other output targets.
  void Write(const AVPacket* packet);

  // Close stops all pushers and finalizes every recording target. The session
  // cannot be opened again after Close.
  void Close() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_OUTPUT_SESSION_H_

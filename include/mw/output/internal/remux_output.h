#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_REMUX_OUTPUT_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_REMUX_OUTPUT_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/performance/pipeline_snapshot.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::performance {
class OperationRecorder;
}

namespace mw::streamer::output::internal {

struct RemuxOutputConfig {
  std::string target;
  zlm::OutputConfig zlm;
  std::size_t startup_packet_capacity = 384;
};

// Validates target syntax and ZLM options without creating execution resources.
void ValidateRemuxOutputConfig(const RemuxOutputConfig& config);

// Single-target output engine. All methods, including destruction, run on the
// supplied poller. The caller owns scheduling, packet ordering and reconnection
// after an input timeline change. This object cannot be reopened after Close.
class RemuxOutput final {
 public:
  // on_failed reports permanent asynchronous failure on the poller, once. It
  // must only notify the owner and must not close or destroy this object
  // inline. Synchronous configuration and write failures are exceptions to the
  // caller.
  // Optional performance storage is borrowed until destruction. Output counts
  // mean packets handed to the local muxer, including startup packets at EOF;
  // they do not acknowledge remote delivery. Calls measure actual packet
  // conversion and muxing, excluding startup metadata discovery and its errors.
  RemuxOutput(RemuxOutputConfig config, std::vector<ffmpeg::StreamInfo> streams,
              std::shared_ptr<toolkit::EventPoller> poller,
              std::function<void(const std::string&)> on_failed,
              performance::OperationRecorder* performance = nullptr);
  ~RemuxOutput();

  RemuxOutput(const RemuxOutput&) = delete;
  RemuxOutput& operator=(const RemuxOutput&) = delete;

  void Open();
  void Write(const ffmpeg::Packet& packet);
  // Writes pending startup packets and finalizes recordings, or throws if
  // metadata is incomplete or the output cannot preserve the submitted data.
  void Finish();
  void Close() noexcept;
  performance::NetworkOutputSnapshot GetNetworkOutputSnapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::output::internal

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_INTERNAL_REMUX_OUTPUT_H_

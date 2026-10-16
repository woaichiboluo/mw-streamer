#ifndef MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_
#define MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/input/av_input_proxy.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace mw::streamer {

struct RemuxPipelineConfig {
  AvInputProxyConfig input;
  RemuxerConfig remuxer;
  // Packet retrieval/delivery speed only. Positive and finite; timestamps and
  // encoded payloads remain unchanged, so file playback duration is unchanged.
  double speed = 1.0;
  // Permitted delivery lead against the scaled source DTS clock, in wall time.
  std::chrono::milliseconds max_ahead{100};
};

enum class RemuxPipelineState {
  kStopped,
  kStarting,
  kRunning,
  kWaitingRetry,
  kDraining,
  kEnded,
  kStopping,
  kFailed,
};

struct RemuxPipelineStatus {
  RemuxPipelineState state = RemuxPipelineState::kStopped;
  AvInputProxyStatus input;
  std::vector<Remuxer::PushStatus> remuxer;
  std::vector<Remuxer::RecordingStatus> recordings;
  // Includes the retirement worker's current session. File IO can delay its
  // completion while a subsequent connection continues delivering packets.
  std::size_t retiring_sessions = 0;
  std::exception_ptr error;
};

// Encoded packet forwarding/recording for a fixed source. Owns AvInputProxy,
// one packet worker, and a retirement worker for closing old recordings.
// Reconnect/loop replaces Remuxer, including network sessions and file names.
// Tracks must retain indices, time bases and codec configuration. Timestamp
// discontinuities in flagged sources also start a new output session.
// Requires Init; Stop/destroy before Shutdown. Serialize Start/Stop/configure/
// output additions externally. Queries, SetSpeed and RequestStop are
// concurrent.
class MW_STREAMER_API RemuxPipeline final {
 public:
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  using OnError = std::function<void(std::exception_ptr)>;

  explicit RemuxPipeline(RemuxPipelineConfig config = {});
  ~RemuxPipeline();
  RemuxPipeline(const RemuxPipeline&) = delete;
  RemuxPipeline& operator=(const RemuxPipeline&) = delete;
  RemuxPipeline(RemuxPipeline&&) = delete;
  RemuxPipeline& operator=(RemuxPipeline&&) = delete;

  // Configure callbacks only before Start/after Stop. OnPacket runs once per
  // paced packet before submission, with original payload/PTS/DTS/time base.
  // Ref() retains storage; callback exceptions fail the pipeline.
  void SetOnPacket(OnPacket callback);
  // First terminal error only, outside locks on the failing worker/caller.
  // Retries and recording failures do not notify. Observer exceptions are
  // contained. Both callbacks may query/RequestStop, but must not Start/Stop,
  // add outputs or destroy this object. Stop waits for all callbacks.
  void SetOnError(OnError callback);

  // Templates survive reconnects/loops; each session generates a new MP4 path.
  // Before first Start returns the planned address. Dynamic additions target
  // the current output and future sessions. Explicit Stop clears templates,
  // retaining the last output/status snapshots. See Remuxer for URL support.
  std::string AddPushUrl(const std::string& url);
  void AddRtspPublish(const std::string& ip, std::uint16_t port,
                      bool tls = false);
  std::string AddHlsPublish(const std::string& ip, std::uint16_t port,
                            bool tls = false);
  Remuxer::OutputInfo output_info() const;

  // Asynchronous startup. True confirms workers started, not a connection.
  // Stop is required before reuse, including after Ended/Failed. Normal EOF
  // drains files and reports Ended; loop EOF opens the next output session.
  // Returns false on invalid URL or worker startup failure; reports OnError.
  [[nodiscard]] bool Start(std::string_view url);
  // Positive finite values only; invalid values throw invalid_argument.
  // Takes effect during a scheduling wait without restarting input/output.
  void SetSpeed(double speed);
  double speed() const;
  RemuxPipelineState state() const;
  AvInputProxyStatus input_status() const;
  std::vector<Remuxer::PushStatus> remuxer_status() const;
  // Retains finalized file snapshots from earlier connections in this run.
  std::vector<Remuxer::RecordingStatus> recording_statuses() const;
  RemuxPipelineStatus status() const;
  std::exception_ptr error() const;
  void RequestStop() noexcept;
  // Wakes packet reads/scheduling, drains accepted output and joins workers.
  // Waits for all recordings: disk IO may delay completion. Ended/Failed and
  // final file snapshots persist until the next Start; no callbacks afterward.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_

#ifndef MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_
#define MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/pipeline/pipeline_state.h"
#include "mw/streamer/remuxer/sync_remuxer.h"

namespace mw::streamer {

struct MW_STREAMER_API RemuxPipelineConfig {
  RemuxPipelineConfig();

  // Defaults to kRemux; other input modes are rejected. Decoder selection and
  // playback_speed are ignored, as in FfmpegInput's kRemux mode.
  FfmpegInputConfig input;
  SyncRemuxerConfig remuxer;
};

struct RemuxPipelineReadyInfo {
  std::vector<ffmpeg::StreamInfo> streams;
  // Actual URLs/recording paths, in AddPushUrl order. Updated by successful
  // runtime additions. These remain queryable after stopping.
  std::vector<std::string> output_urls;
  std::string rtsp_url;
  std::string hls_url;
};

// One independent Input(kRemux) -> SyncRemuxer session. Requires Init before
// Start and destruction before Shutdown. Configure callbacks before Start.
// Callbacks run on internal threads (or the Start/Stop caller), may query
// snapshots, and must not call lifecycle/configuration methods or destroy this
// object. Callback exceptions are caught and logged.
class MW_STREAMER_API RemuxPipeline final {
 public:
  using OnReady = std::function<void(const RemuxPipelineReadyInfo&)>;
  using OnStateChanged = std::function<void(const PipelineStatus&)>;

  explicit RemuxPipeline(RemuxPipelineConfig config = {});
  ~RemuxPipeline();
  RemuxPipeline(const RemuxPipeline&) = delete;
  RemuxPipeline& operator=(const RemuxPipeline&) = delete;
  RemuxPipeline(RemuxPipeline&&) = delete;
  RemuxPipeline& operator=(RemuxPipeline&&) = delete;

  void SetOnReady(OnReady callback);
  void SetOnStateChanged(OnStateChanged callback);

  // Before Start, register outputs without opening files or listening. During
  // operation, activate them synchronously. Stop clears registered outputs;
  // add them again before another Start. AddPushUrl returns the current or
  // next session's planned path. Query ready_info after Start for that session.
  // Repeated push targets are shared. Add calls are rejected while stopping.
  std::string AddPushUrl(const std::string& url);
  // One RTSP and one HLS publishing point per session. TLS uses Init's server
  // certificate; tls=false selects RTSP/HTTP, true selects RTSPS/HTTPS.
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port,
                      bool tls = false);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls = false);

  // Waits for input opening, track/output initialization and the first accepted
  // packet. OnReady runs before that packet. A prepared input with no packets
  // may finish successfully at EOF. Short inputs may already have ended when
  // this returns. Startup failures finish cleanup before throwing.
  // Input timeouts/retry policy apply during startup, too. Start calls must be
  // serialized; Stop from another thread may end the startup wait.
  void Start(std::string_view url);
  // Waits for Input's in-flight submission while Remuxer continues consuming,
  // then drains/stops Remuxer. No callbacks run after return. Repeated Stop is
  // harmless and preserves an already-ended/failed state. Another Start is
  // allowed. Fatal errors and non-loop EOF use this same cleanup sequence.
  void Stop();

  PipelineState state() const noexcept;
  PipelineStatus status() const;
  RemuxPipelineReadyInfo ready_info() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_REMUX_PIPELINE_H_

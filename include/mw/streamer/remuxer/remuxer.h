#ifndef MW_STREAMER_REMUXER_REMUXER_H_
#define MW_STREAMER_REMUXER_REMUXER_H_

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/recorder/recorder.h"

namespace mw::streamer {

struct RemuxerConfig {
  // Shared by all protocol publications in this session. Single ASCII URI
  // segments; an empty stream generates a unique name for this instance.
  std::string app = "live";
  std::string stream;
  // Per local MP4 target; overflow drops the current packet with a WARN.
  std::size_t recording_queue_bytes = 64 * 1024 * 1024;
};

// One encoded audio/video session and its dynamically added outputs. Requires
// Init before Start; stop upstream delivery and destroy this before Shutdown.
// Error callbacks run on a ZLM poller and must not call Start, Stop or add
// outputs. Callback exceptions are logged. Serialize control calls externally.
class MW_STREAMER_API Remuxer final {
 public:
  // Planned addresses before Start and actual addresses during/after the
  // session. Stop retains this snapshot while clearing configured outputs.
  struct OutputInfo {
    std::vector<std::string> output_urls;
    std::string rtsp_url;
    std::string hls_url;
  };

  struct PushStatus {
    std::string url;
    // Raw PusherProxy status: 0 is normal, 1 is attempting to push.
    // The SDK may retain 0 briefly after disconnection; this is not proof
    // of continuous packet delivery or successful remote playback.
    int status = 1;
  };

  struct RecordingStatus {
    std::string path;
    RecorderStatus status;
  };

  // Once per terminal session failure, on the output poller outside locks.
  // Stops accepting packets before notifying and closes the session afterward.
  // Preserves processing exceptions; unrecoverable ZLM output errors are
  // wrapped in runtime_error with their target, code and message. Recoverable
  // network failures are logged and retried internally without invoking this
  // callback. Startup failures log and return false; output registration
  // failures throw directly. Neither synchronous failure invokes this callback.
  // File recording failures are isolated and reported by recording_statuses().
  using OnError = std::function<void(std::exception_ptr)>;

  Remuxer();
  explicit Remuxer(RemuxerConfig config);
  ~Remuxer();
  Remuxer(const Remuxer&) = delete;
  Remuxer& operator=(const Remuxer&) = delete;
  Remuxer(Remuxer&&) = delete;
  Remuxer& operator=(Remuxer&&) = delete;

  void SetOnError(OnError callback);

  // Use AvEncoder::streams() encoded tracks, not the original input tracks.
  // Supports H264/H265 video and AAC audio, at most one track of each type.
  // Initializes tracks and registered outputs synchronously. Returns true on
  // success, false on failure after rollback; configured outputs remain for
  // retry. A repeated Start returns false without changing the active session.
  // Network connections/retries are asynchronous; success only confirms local
  // initialization, not a remote connection.
  [[nodiscard]] bool Start(
      const std::vector<ffmpeg::StreamInfo>& streams) noexcept;
  // Retains native Encoder PTS/DTS and time base. The one-packet handoff
  // blocks until the worker consumes its previous packet. Drain/Stop rejects
  // blocked submissions; accepted packets are submitted to the native muxer
  // in order. Protocol buffering and network delivery are managed by ZLM.
  // Concurrent calls are safe, but each track must arrive in DTS order.
  // Calling on this remuxer's output poller throws to prevent self-wait.
  bool SubmitPacket(const ffmpeg::Packet& packet);
  // Before Start, registers an output without creating resources; during a
  // session, activates it synchronously. Network URLs retry automatically.
  // Local .mp4 targets use independent asynchronous fMP4 Recorders; .m3u8
  // recording is unsupported. Parent directories must exist. Paths gain
  // _YYYY_MM_DD_HH_MM_SS_id before their extension. Returns the actual
  // URL/path, which Start uses unchanged. Repeated URLs/paths share the output.
  // Draining sessions reject additions. Queue overflow/file errors do not fail
  // Remuxer. Dynamic recordings take subsequent packets without keyframe
  // selection.
  std::string AddPushUrl(const std::string& url);
  // One listener per protocol. Both publications use RemuxerConfig's shared
  // app/stream. May be registered before Start or activated while running.
  // Same IP/port shares a listener across Remuxers. TLS requires the runtime's
  // configured certificate. Active sessions cannot share the same app/stream.
  void AddRtspPublish(const std::string& ip, std::uint16_t port);
  void AddRtspPublish(const std::string& ip, std::uint16_t port, bool tls);
  // Native ZLM HLS: rolling TS segments, served from the configured HTTP root.
  // Returns http(s)://IP:port/app/stream/hls.m3u8. HLS generation and cleanup
  // use ZLM's native implementation on the output poller.
  // Invalid parameters throw on Add; listener/certificate errors throw when
  // added while active, or make Start return false during startup.
  std::string AddHlsPublish(const std::string& ip, std::uint16_t port);
  std::string AddHlsPublish(const std::string& ip, std::uint16_t port,
                            bool tls);
  // Thread-safe snapshot, also available from callbacks. Before Start these
  // are planned addresses. Stop preserves the snapshot until new additions
  // or Start, while clearing the configured outputs.
  OutputInfo output_info() const;
  // Queries the output poller synchronously. Returns all currently created
  // network pushers in registration order. Pending targets, local recordings
  // and RTSP/HLS publications are excluded. Empty before Start/after closure.
  // Safe concurrently with control calls and from error callbacks.
  std::vector<PushStatus> push_statuses() const;
  // Independent local file snapshots in registration order. Empty before the
  // first successful Start. Final states remain after Stop and are replaced on
  // the next successful Start. Safe concurrently and from error callbacks;
  // does not wait for the output poller or file IO.
  std::vector<RecordingStatus> recording_statuses() const;
  // Rejects new packets/targets and drains accepted packets.
  // Asynchronously requests file finalization. Live network delivery is
  // managed by ZLM.
  void Drain() noexcept;
  // Drains accepted packets and waits for network outputs/listeners to close,
  // so another instance can reuse the same app/stream and publication ports.
  // Requests asynchronous Recorder finalization without waiting for file IO.
  // Clears configured outputs and preserves status/address snapshots, as Stop
  // does. Keep this instance alive until Stop/destruction joins its Recorders.
  // Serialize lifecycle calls externally; do not call from callbacks.
  void StopProtocols() noexcept;
  // Drains synchronously and waits for completion, outside callbacks. Another
  // Start begins a new session with a fresh timeline. Stop clears configured
  // outputs, including any added before Start; add them again before
  // restarting. Waits for all file Recorders outside the ZLM poller; disk IO
  // can delay this call without occupying the poller.
  void Stop() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_REMUXER_REMUXER_H_

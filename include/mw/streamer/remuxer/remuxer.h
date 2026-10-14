#ifndef MW_STREAMER_REMUXER_REMUXER_H_
#define MW_STREAMER_REMUXER_REMUXER_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

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

  // Once per terminal session failure, on the output poller outside locks.
  // Stops accepting packets before notifying and closes the session afterward.
  // Preserves processing exceptions; unrecoverable ZLM output errors are
  // wrapped in runtime_error with their target, code and message. Recoverable
  // network failures are logged and retried internally without invoking this
  // callback. Startup failures log and return false; output registration
  // failures throw directly. Neither synchronous failure invokes this callback.
  using OnError = std::function<void(std::exception_ptr)>;

  Remuxer();
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
  // blocked submissions; accepted packets are drained without dropping.
  // Concurrent calls are safe, but each track must arrive in DTS order.
  // Calling on this remuxer's output poller throws to prevent self-wait.
  bool SubmitPacket(const ffmpeg::Packet& packet);
  // Before Start, registers an output without creating resources; during a
  // session, activates it synchronously. Network URLs retry automatically.
  // Local .mp4/.m3u8 paths gain _YYYY_MM_DD_HH_MM_SS_id before their extension.
  // Returns the actual URL/planned path, which Start uses unchanged. Repeated
  // URLs/paths share the registered output. Draining sessions reject additions.
  std::string AddPushUrl(const std::string& url);
  // One local publishing point per session. Same IP/port shares a listener
  // across Remuxers; app/stream must be unique within the process.
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port);
  // TLS publication requires InitConfig.tls.certificate_file.
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls);
  // May be registered before Start or activated afterward. One HTTP HLS
  // publication per session, independent of RTSP.
  // Returns http://IP:port/app/stream/hls.m3u8 with rolling TS segments.
  // Same IP/port shares a listener; HLS app/stream is unique in the process.
  // Parameter errors throw on Add. Binding/certificate errors throw when
  // activated during operation; a binding/certificate failure during Start
  // makes Start return false.
  // HLS uses ZLM's configured HTTP root. Live HLS files are cleaned
  // synchronously when this session ends.
  // app/stream must be single ASCII URI segments.
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port);
  // true publishes HTTPS; false keeps HTTP. TLS requires a server certificate.
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls);
  // Thread-safe snapshot, also available from callbacks. Before Start these
  // are planned addresses. Stop preserves the snapshot until new additions
  // or Start, while clearing the configured outputs.
  OutputInfo output_info() const;
  // Rejects new packets/targets, drains accepted packets and finalizes files.
  // Live network delivery is managed by ZLM.
  void Drain() noexcept;
  // Drains synchronously and waits for completion, outside callbacks. Another
  // Start begins a new session with a fresh timeline. Stop clears configured
  // outputs, including any added before Start; add them again before
  // restarting.
  void Stop() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_REMUXER_REMUXER_H_

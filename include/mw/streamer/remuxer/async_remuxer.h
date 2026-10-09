#ifndef MW_STREAMER_REMUXER_ASYNC_REMUXER_H_
#define MW_STREAMER_REMUXER_ASYNC_REMUXER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

// One encoded audio/video session and its dynamically added outputs. Requires
// Init before Start; stop upstream delivery and destroy this before Shutdown.
// Callbacks run on a ZLM poller, must consume their own exceptions, and must
// not call Start, Stop or add outputs. Serialize control calls externally.
class MW_STREAMER_API AsyncRemuxer final {
 public:
  // An empty target identifies a session error; other errors identify a URL.
  using OnError = std::function<void(std::string_view target, int error,
                                     std::string_view message)>;
  using OnEnded = std::function<void()>;

  AsyncRemuxer();
  ~AsyncRemuxer();
  AsyncRemuxer(const AsyncRemuxer&) = delete;
  AsyncRemuxer& operator=(const AsyncRemuxer&) = delete;
  AsyncRemuxer(AsyncRemuxer&&) = delete;
  AsyncRemuxer& operator=(AsyncRemuxer&&) = delete;

  void SetOnError(OnError callback);
  void SetOnEnded(OnEnded callback);

  // Use Encoder::OnReady's encoded tracks, not the original input tracks.
  // Supports H264/H265 video and AAC audio, at most one track of each type.
  void Start(const std::vector<ffmpeg::StreamInfo>& streams);
  // Safe for concurrent Encoder::OnPacket calls. Retains the packet; dts_ns is
  // Encoder's synchronized ordering clock. No network congestion dropping.
  bool SubmitPacket(const ffmpeg::Packet& packet, std::int64_t dts_ns);
  // Start first. Network URLs retry automatically. Local .mp4/.m3u8 paths gain
  // _YYYY_MM_DD_HH_MM_SS before their extension. Returns the actual URL/path.
  // Targets remain until this session ends; repeated network URLs are shared.
  std::string AddPushUrl(const std::string& url);
  // One local publishing point per session. Same IP/port shares a listener
  // across AsyncRemuxers; app/stream must be unique within the process.
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port);
  // Start first. One HTTP HLS publication per session, independent of RTSP.
  // Returns http://IP:port/app/stream/hls.m3u8 with rolling TS segments.
  // Same IP/port shares a listener; HLS app/stream is unique in the process.
  // Invalid arguments or occupied paths/listeners throw synchronously.
  // HLS uses ZLM's configured HTTP root. Live HLS files are cleaned
  // synchronously when this session ends.
  // app/stream must be single ASCII URI segments.
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port);
  // Rejects new packets/targets, drains accepted packets and finalizes files.
  // OnEnded fires once. Live network delivery is managed by ZLM.
  void Drain() noexcept;
  // Drains synchronously and waits for completion, outside callbacks. Another
  // Start begins a new session with an empty target list and a fresh timeline.
  void Stop() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_REMUXER_ASYNC_REMUXER_H_

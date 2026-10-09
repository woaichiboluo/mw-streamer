#ifndef MW_STREAMER_REMUXER_SYNC_REMUXER_H_
#define MW_STREAMER_REMUXER_SYNC_REMUXER_H_

#include <cstddef>
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

struct MW_STREAMER_API SyncRemuxerConfig {
  // Retained payload and side-data bytes, including startup and processing.
  // Output SDK buffers are not included. A larger single packet is rejected.
  std::size_t max_pending_bytes = 64 * 1024 * 1024;
};

// Original compressed input recording with bounded asynchronous consumption.
// SubmitPacket waits only for buffer space, not for writing or peer receipt.
// Requires Init before Start and destruction before Shutdown. Serialize input
// packet calls in demux order, and serialize Start/Stop/output control calls.
// Drain may run concurrently with a blocked SubmitPacket. Callbacks execute on
// the worker or ZLM poller; they must not throw, block, call Stop/Start, or add
// outputs. Stop upstream delivery before destroying this object.
class MW_STREAMER_API SyncRemuxer final {
 public:
  using OnError = std::function<void(std::string_view target, int error,
                                     std::string_view message)>;
  using OnEnded = std::function<void()>;

  explicit SyncRemuxer(SyncRemuxerConfig config = {});
  ~SyncRemuxer();
  SyncRemuxer(const SyncRemuxer&) = delete;
  SyncRemuxer& operator=(const SyncRemuxer&) = delete;
  SyncRemuxer(SyncRemuxer&&) = delete;
  SyncRemuxer& operator=(SyncRemuxer&&) = delete;

  void SetOnError(OnError callback);
  void SetOnEnded(OnEnded callback);

  // Supports one H264/H265 video and one AAC audio track, with complete codec
  // configuration. Uses the original input tracks from Input::OnReady.
  void Start(const std::vector<ffmpeg::StreamInfo>& streams);
  // Retains packet buffers without modifying the source. PTS/DTS must exist,
  // and DTS must strictly increase per track within each generation. An unset
  // packet time_base (zero numerator, nonnegative denominator) uses its
  // declared stream time_base; a malformed or mismatching time_base is invalid.
  // Each increasing generation starts at the previous generation's maximum
  // PTS/DTS end, preserving source audio/video offsets with one common origin.
  // Startup waits for each track and a video keyframe; insufficient startup
  // space terminates the session rather than deadlocking. Drain allows absent
  // tracks. Exceeding the output SDK's unready-track cache also fails
  // explicitly. Throws invalid_argument for invalid packets, length_error for
  // an oversized packet; returns false after Drain/Stop or a session failure.
  bool SubmitPacket(std::uint64_t generation, const ffmpeg::Packet& packet);
  bool SubmitPacket(std::uint64_t generation, const AVPacket& packet);
  // Local MP4/M3U8 paths gain a timestamp suffix. Network URLs retry. Returns
  // the actual path/URL. Add outputs before submitting packets to record them.
  std::string AddPushUrl(const std::string& url);
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
  // Reject new submissions, unblock waiting producers, and drain
  // asynchronously. OnEnded fires once after the accepted data and outputs have
  // been finalized.
  void Drain() noexcept;
  // Drain, finalize files, and wait for the worker. Never call from callbacks.
  // A subsequent Start creates a fresh session with no output targets.
  void Stop() noexcept;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_REMUXER_SYNC_REMUXER_H_

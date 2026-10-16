#ifndef MW_STREAMER_RECORDER_RECORDER_H_
#define MW_STREAMER_RECORDER_RECORDER_H_

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

namespace mw::streamer {

struct RecorderConfig {
  // Local .mp4 file, overwritten on Start. The parent directory must exist.
  // Only fragmented MP4 is supported; URLs and .m3u8 are rejected.
  std::string path;
  // Accounts for queued packet headers, payloads and side data. Excludes the
  // packet being written and FFmpeg's internal muxing/IO buffers.
  std::size_t max_queue_bytes = 64 * 1024 * 1024;
};

enum class RecorderState { kStopped, kRunning, kStopping, kFailed };

struct RecorderStatus {
  RecorderState state = RecorderState::kStopped;
  std::size_t queue_bytes = 0;
  // Successfully submitted to the file muxer; not a durable-on-disk count.
  std::uint64_t written_packets = 0;
  // Queue overflow only. Rejected calls outside kRunning are not counted.
  std::uint64_t dropped_packets = 0;
  std::string error;
};

// Standalone asynchronous encoded-packet recorder; does not require Init or
// depend on Remuxer. File failures affect only this instance.
// Serialize Start/Stop/destruction externally. Submission and status queries
// are thread-safe, including concurrently with Start/Stop. Stop before reuse.
class MW_STREAMER_API Recorder final {
 public:
  using OnError = std::function<void(std::exception_ptr)>;

  Recorder();
  ~Recorder();
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;

  // Optional notification, once per failed session on the writer thread,
  // after kFailed/error are stored and the pending queue is released. Preserves
  // the original exception. Runs outside locks; may query status or replace
  // the callback, but must not Start/Stop/destroy this Recorder. Exceptions
  // thrown by the callback are logged and contained. Thread-safe replacement
  // affects future notifications; an already selected callback may still run.
  // Queue overflow and synchronous Start/parameter failures do not notify.
  // Stop waits for the callback to return. Registration survives restarts.
  void SetOnError(OnError callback);

  // Supports H264/H265 video and AAC audio, at most one of each. Copies track
  // descriptions. Invalid config/tracks throw invalid_argument. False if the
  // previous worker has not been joined or a new thread could not be started.
  // True means packets may be queued, not that the file has opened: opening,
  // all writes and finalization run on the worker and report failure in status.
  // A new session resets statistics and error, including after a failed Stop.
  [[nodiscard]] bool Start(const std::vector<ffmpeg::StreamInfo>& streams,
                           RecorderConfig config);
  // Briefly locks to retain a packet reference and enqueue it; never waits for
  // queue space or file IO. Payloads must remain immutable. Each track must be
  // submitted in increasing DTS order; StreamInfo.time_base is authoritative.
  // Uses a common presentation origin for all tracks, retaining their relative
  // timing, video reorder delay and initial audio padding.
  // Unknown tracks/empty packets throw invalid_argument while running.
  // Returns false when not running or full. Full queues drop the current
  // packet with a rate-limited WARN; no keyframe recovery or timestamp repair.
  bool TrySubmitPacket(const ffmpeg::Packet& packet);
  RecorderStatus status() const;
  // Rejects submissions and asks the writer to drain/finalize asynchronously.
  // Safe concurrently with submissions/status. Stop still joins the worker.
  void Drain() noexcept;
  // Rejects submissions, drains accepted packets and joins the writer. Can
  // wait for disk IO; no forced cancellation. Failed state/error are retained.
  // Failures clear the pending queue and close the file best-effort; partial
  // files are retained. Successful Stop keeps counters with kStopped state.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_RECORDER_RECORDER_H_

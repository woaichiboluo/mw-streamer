#ifndef MW_STREAMER_INPUT_AV_INPUT_H_
#define MW_STREAMER_INPUT_AV_INPUT_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

struct AvInputConfig {
  // Maximum queued packets, shared by selected streams. Must be positive.
  // FFmpeg's own probing and protocol buffers are outside this limit.
  std::size_t packet_capacity = 256;
  std::chrono::milliseconds open_timeout{60000};
  std::chrono::milliseconds read_timeout{60000};
  // Empty means automatic format detection.
  std::string input_format_name;
  // Passed to avformat_open_input. Unused options are logged as warnings.
  std::map<std::string, std::string> options;
};

enum class AvInputState {
  kClosed,
  kOpening,
  kOpened,
  // Native EOF and the packet queue has been completely consumed. Does not
  // mean the consumer has finished delivering its last returned packet.
  kEnded,
  kFailed,
  kCancelled,
};

struct AvInputStatus {
  AvInputState state = AvInputState::kClosed;
  // The original exception belongs to the connection controller. Non-null
  // only for kFailed; retaining it preserves its type and lifetime.
  std::exception_ptr error;
};

struct AvInputInfo {
  // Deep copies of all source streams, including non-audio/video streams.
  std::vector<ffmpeg::StreamInfo> streams;
  // First video/audio stream in source order, regardless of disposition.
  // Original stream indices are preserved; -1 means the type is absent.
  int video_stream_index = -1;
  int audio_stream_index = -1;
  std::int64_t start_time_us = AV_NOPTS_VALUE;
  std::int64_t duration_us = AV_NOPTS_VALUE;
  std::map<std::string, std::string> metadata;
  // Mirrors the demuxer's AVFMT_TS_DISCONT capability (e.g. HLS/MPEG-TS).
  // Raw AvInput packets retain native timestamps; Live consumers may correct
  // discontinuities before decoding/filtering.
  bool timestamp_discontinuities = false;
};

enum class AvInputReadStatus { kPacket, kEof, kNull };

struct AvInputReadResult {
  AvInputReadStatus status = AvInputReadStatus::kNull;
  // Present only for kPacket. Owns a reference independent of AvInput.
  std::optional<ffmpeg::Packet> packet;
};

// One demux connection and one reader worker with a bounded packet queue.
// Open/Close/configuration must be serialized externally. Exactly one packet
// consumer may run concurrently with the reader and Close. Snapshots and
// RequestCancel are thread-safe. Delivers only the first video and first audio
// stream. No decoding or retry policy.
// Open must not overlap packet reads; finish the old consumer before reopening.
class MW_STREAMER_API AvInput final {
 public:
  // Notifications run on the Open/Close caller or reader worker, outside locks.
  // Observers may query snapshots or request cancellation, but must not call
  // ReadPacket/Open/Close/configuration or destroy this object. Exceptions are
  // logged. Opened means probing succeeded, not that prefetch already began.
  using OnStateChanged = std::function<void(const AvInputStatus&)>;

  // Invalid capacity or nonpositive timeouts throw std::invalid_argument.
  explicit AvInput(AvInputConfig config = {});
  ~AvInput();
  AvInput(const AvInput&) = delete;
  AvInput& operator=(const AvInput&) = delete;
  AvInput(AvInput&&) = delete;
  AvInput& operator=(AvInput&&) = delete;

  void SetOnStateChanged(OnStateChanged callback);
  // Synchronously opens and probes, then starts prefetch. May be cancelled
  // from another thread. Failure reports status and throws; call Close before
  // another Open. The opening timeout covers both opening and probing.
  void Open(std::string_view url);
  // Waits for a packet or a terminal connection state. Normal EOF returns kEof
  // after draining the queue. Failure drains the queue then returns kNull;
  // cancellation/Close return kNull immediately. Source errors are reported
  // only through status/OnStateChanged, never through packet reads.
  // Packet timing/flags/side data are unchanged from av_read_frame.
  AvInputReadResult ReadPacket();
  // Also returns kNull immediately when an active source has no queued packet.
  AvInputReadResult TryReadPacket();
  // Same result semantics as ReadPacket. Timeout returns kNull without
  // cancelling I/O or changing connection state; cancellation wakes promptly.
  // Zero is equivalent to TryReadPacket; negative values throw
  // invalid_argument.
  AvInputReadResult ReadPacketFor(std::chrono::milliseconds timeout);
  // Cooperative cancellation of I/O and queue waits, without joining.
  void RequestCancel() noexcept;
  // Cancels, joins and discards queued packets. No callbacks after returning.
  // Already returned packets and info snapshots remain valid. Idempotent.
  void Close() noexcept;

  AvInputStatus status() const;
  AvInputInfo info() const;
  std::size_t buffered_packets() const;

 private:
  friend class AvInputProxy;
  AvInputReadResult ReadPacketUntil(
      std::optional<std::chrono::steady_clock::time_point> deadline);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_INPUT_H_

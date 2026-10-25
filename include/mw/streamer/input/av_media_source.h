#ifndef MW_STREAMER_INPUT_AV_MEDIA_SOURCE_H_
#define MW_STREAMER_INPUT_AV_MEDIA_SOURCE_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/input/av_input_proxy.h"

namespace mw::streamer {

struct AvMediaSourceConfig {
  AvInputProxyConfig proxy;
  AvDecoderConfig video_decoder;
  AvDecoderConfig audio_decoder;
};

enum class AvMediaSourceState {
  kStopped,
  kConnecting,
  // Selected decoders are initialized; frames may not be available yet.
  kRunning,
  kWaitingRetry,
  // Final packet EOF was submitted; consumers must drain both selected tracks.
  kDraining,
  kEnded,
  kFailed,
};

struct AvMediaSourceStatus {
  AvMediaSourceState state = AvMediaSourceState::kStopped;
  std::uint64_t retry_count = 0;
  std::chrono::seconds retry_delay{0};
  // Recoverable connection failure in WaitingRetry, terminal failure in Failed.
  std::exception_ptr error;
  std::shared_ptr<const AvInputInfo> info;
};

// Owns an input proxy and one decoder per selected track. Decoders own the
// frame queues; this class adds no frame queue, filtering or clock mapping.
// Dispatch blocks on bounded decoder input queues, including ordered EOF/reset
// submission to all present tracks. One stalled track may delay the other.
// Stop or a terminal input/decoder failure wakes blocked delivery to both
// tracks. Recoverable connection events wait for blocked delivery to resume
// before the dispatcher invokes their state observers. They do not discard
// queued media. Empty packet reads block until data, connection events or
// cancellation. Decoder backpressure is handled inside its queue; final EOF
// acknowledgement waits for the consumers to read both tracks' EOF items.
// Lifecycle/configuration calls must be serialized externally. One reader per
// track may run concurrently with the other track and Stop. Consume every
// present track to relieve backpressure. Stop and finish readers before restart
// or destruction. Reconnects must retain the selected track indices/format;
// incompatible source descriptions fail rather than silently change decoders.
class MW_STREAMER_API AvMediaSource final {
 public:
  // Serial notifications on the controller worker, outside locks. Callbacks may
  // query/read/RequestStop, but must not Start/Stop/configure/destroy the
  // source. Callback exceptions are logged. Decoder errors are terminal source
  // failures.
  using OnStateChanged = std::function<void(const AvMediaSourceStatus&)>;
  // Observes each input packet once before decoder submission, on the
  // controller worker outside locks. Packet timestamps, time base and stream
  // index are unchanged; Ref() to retain storage. Must return promptly. May
  // query status/info or RequestStop, but must not change configuration,
  // Start/Stop or destroy the source. Exceptions fail the source.
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;

  // Invalid input/retry/decoder configuration throws synchronously.
  explicit AvMediaSource(AvMediaSourceConfig config = {});
  // Retains the caller's device reference. CUDA video remains on the device;
  // audio is decoded on CPU. No device is created and no software fallback is
  // attempted. Frames retain their storage/device beyond Stop or destruction.
  AvMediaSource(const ffmpeg::HwDeviceContext& device,
                AvMediaSourceConfig config = {});
  ~AvMediaSource();
  AvMediaSource(const AvMediaSource&) = delete;
  AvMediaSource& operator=(const AvMediaSource&) = delete;
  AvMediaSource(AvMediaSource&&) = delete;
  AvMediaSource& operator=(AvMediaSource&&) = delete;

  // Register only before Start or after Stop.
  void SetOnStateChanged(OnStateChanged callback);
  // Register only before Start or after Stop.
  void SetOnPacket(OnPacket callback);
  // Thread-safe weak wakeup slot for the sole frame consumer. Decoder queues
  // and source state changes notify it directly. Binding wakes it to rescan
  // current frames/status; a notification does not guarantee a readable frame.
  void SetReadSignal(std::weak_ptr<internal::WaitSignal> signal);
  // Starts asynchronously. Invalid URL/restart/thread creation failure returns
  // false and logs; connection/decode failures are reported through status.
  // Call Stop before another Start.
  [[nodiscard]] bool Start(std::string_view url) noexcept;
  // Independent nonblocking FIFO reads. Preserve decoded pixels/samples and
  // best-effort source PTS/time base, including jumps and inter-track offsets.
  // Absent tracks, underflow, opening/retry, Stop or failure may return kNull;
  // cached frames remain readable during retries. info() identifies absent
  // tracks. Normal EOF follows each track's delayed frames once per input;
  // looping reopens the input. Reconnects reset decoder state in packet order
  // without synthesizing an EOF. EOF never stops an external Live
  // output. Final Ended requires reading EOF from all present tracks.
  AvDecoderReadResult TryReadVideo();
  AvDecoderReadResult TryReadAudio();
  AvMediaSourceStatus status() const;
  // Latest connection description; read results carry their own stream index
  // and time base. Returned snapshots outlive the source.
  std::shared_ptr<const AvInputInfo> info() const;

  // Cooperative cancellation, safe from callbacks; does not join.
  void RequestStop() noexcept;
  // Cancels I/O and queue waits, joins all workers and releases connections.
  // No callbacks after return. Final Ended/Failed status is retained.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_MEDIA_SOURCE_H_

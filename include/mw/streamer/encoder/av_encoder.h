#ifndef MW_STREAMER_ENCODER_AV_ENCODER_H_
#define MW_STREAMER_ENCODER_AV_ENCODER_H_

#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

// One bounded input queue and one owned worker per enabled track. Audio/video
// admission and encoding advance independently, preserving per-track FIFO.
// This component does not mux packets or create hardware devices. Serialize
// configuration and Start/Stop externally; stop producers before destruction.
class MW_STREAMER_API AvEncoder final {
 public:
  // Called on each track's encoding worker, concurrently across audio/video.
  // Calls for the same track are serial. The packet owns encoder timestamps,
  // time_base and its assigned stream_index; Ref() to retain it. A callback
  // exception fails the session and reaches OnError.
  // Callbacks may query streams/error, but must not Start/Stop/destroy or
  // submit frames on this instance. Set callbacks before Start or after Stop.
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  // Once per failed session, on the first failing worker outside locks. Failure
  // closes both tracks; queued work is abandoned and in-flight work may finish.
  // The callback has the same restrictions as OnPacket. Observer exceptions
  // are logged; the original exception remains in error(). Initialization
  // failures return false from Start and do not invoke this callback.
  using OnError = std::function<void(std::exception_ptr)>;

  explicit AvEncoder(const ffmpeg::HwDeviceContext& device);
  ~AvEncoder();
  AvEncoder(const AvEncoder&) = delete;
  AvEncoder& operator=(const AvEncoder&) = delete;
  AvEncoder(AvEncoder&&) = delete;
  AvEncoder& operator=(AvEncoder&&) = delete;

  void SetOnPacket(OnPacket callback);
  void SetOnError(OnError callback);
  // Copies the config/layout, opens enabled codecs/filters synchronously, then
  // starts one worker per enabled track. NONE disables that track; both
  // disabled is invalid. Video dimensions/layout must be explicit. Hardware
  // codecs require a matching shared device; no replacement device or codec is
  // attempted. On failure, logs, retains the exception in error(), releases
  // partial resources and returns false; a subsequent Start may retry. Repeated
  // Start before Stop returns false without changing the active session.
  [[nodiscard]] bool Start(const AvEncoderConfig& config) noexcept;
  // Thread-safe owned snapshot of actual encoded tracks (video 0, audio 1).
  // Available immediately after Start, retained after Stop, empty before Start
  // or after a failed initialization. It never describes source tracks.
  std::vector<ffmpeg::StreamInfo> streams() const;
  // One producer per track may run concurrently with the other producer and
  // Stop. References read-only media buffers in the configured format, with
  // valid PTS/time_base and nonnegative duration. The caller (normally Syncer)
  // supplies timing; no timestamp generation, normalization or discontinuity
  // correction occurs here. Only converts time-base units for codecs/audio
  // reblocking, retaining encoder-native
  // delay/reordering in output packets. Each track queues one frame besides
  // its active work. Never waits for queue space. Full queues reject/drop the
  // incoming frame, log a warning and return false; not started/stopped/failed
  // also returns false. Rejection leaves the caller's frame unchanged.
  // Disabled tracks, invalid payload, audio format or timing throw
  // synchronously. Video layout/device validation and transfers run on the
  // worker; their failures reach OnError. Allocation errors may throw.
  bool TryPushVideo(const ffmpeg::Frame& frame);
  bool TryPushAudio(const ffmpeg::Frame& frame);
  std::exception_ptr error() const;
  // Rejects new frames, drains both accepted FIFOs independently, then joins
  // both workers. The audio worker closes its filter and drains its samples;
  // both workers send codec EOF and receive all delayed packets. Failure
  // abandons pending work. Destruction does the same. Captured callback
  // resources must outlive completion. No callbacks run after Stop returns.
  // Callbacks and device survive for another Start.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_AV_ENCODER_H_

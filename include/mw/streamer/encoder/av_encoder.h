#ifndef MW_STREAMER_ENCODER_AV_ENCODER_H_
#define MW_STREAMER_ENCODER_AV_ENCODER_H_

#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/media_frame.h"

namespace mw::streamer {

// One bounded frame queue and one owned worker. Each frame's audio/video tasks
// run through TBB and finish before taking the next frame. This component does
// not mux packets or create hardware devices. Serialize configuration and
// Start/Stop externally; stop producers before destruction.
class MW_STREAMER_API AvEncoder final {
 public:
  // Called on encoding tasks, possibly concurrently for audio/video. The packet
  // owns encoder timestamps/time_base and its assigned stream_index; Ref() to
  // retain it. A callback exception fails the session and reaches OnError.
  // Callbacks may query streams/error, but must not Start/Stop/destroy or
  // submit frames on this instance. Set callbacks before Start or after Stop.
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  // Once per terminal worker failure, outside locks. Observer exceptions are
  // logged; the original exception remains in error(). Initialization failures
  // return false from Start and do not invoke this callback.
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
  // starts the frame worker. NONE disables that track; both disabled is
  // invalid. Video dimensions/layout must be explicit. Hardware codecs require
  // a matching shared device; no replacement device or codec is attempted. On
  // failure, logs, retains the exception in error(), releases partial resources
  // and returns false; a subsequent Start may retry. Repeated Start before Stop
  // returns false without changing the active session.
  [[nodiscard]] bool Start(const AvEncoderConfig& config) noexcept;
  // Thread-safe owned snapshot of actual encoded tracks (video 0, audio 1).
  // Available immediately after Start, retained after Stop, empty before Start
  // or after a failed initialization. It never describes source tracks.
  std::vector<ffmpeg::StreamInfo> streams() const;
  // References read-only media buffers. Input must contain exactly the enabled
  // tracks in the configured format. Source PTS is not the encoding clock:
  // video frames/audio samples are counted at admission, including dropped
  // frames. The realtime queue holds one complete frame besides active work.
  // Full queues drop the incoming frame and return true; closed/failed returns
  // false. Track presence, nonempty payload and audio format validations throw
  // synchronously. Video layout/device validation and transfers run on the
  // worker; their failures reach OnError. Allocation errors may throw.
  bool SubmitFrame(const MediaFrame& frame);
  std::exception_ptr error() const;
  // Rejects new frames, queues an EOF marker behind accepted work, then joins.
  // The worker closes the audio filter, drains its samples, sends codec EOF and
  // receives all delayed packets. Failure abandons pending work. Destruction
  // does the same. Captured callback resources must outlive completion; after
  // returning no callbacks run. Callbacks and device survive for another Start.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_AV_ENCODER_H_

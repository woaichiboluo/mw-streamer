#ifndef MW_STREAMER_SYNCER_SYNCER_H_
#define MW_STREAMER_SYNCER_SYNCER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/encoder/av_encoder_config.h"

namespace mw::streamer {

// Continuity policy, independent of encoder formats and standby storage.
struct SyncerOptions {
  std::chrono::microseconds timeout{1000000};
  std::chrono::microseconds freeze{5000000};
  std::chrono::microseconds max_streams_diff{1000};
  std::chrono::microseconds start_ts{10000000};
  // False seeds each track's expected PTS with start_ts, filling positive
  // initial gaps and correcting negative offsets.
  // True leaves it unset until the first frame or input timeout.
  bool forward_start_shift = false;
};

struct SyncerConfig {
  // Formats determine enabled tracks and the default backup/silence formats;
  // NONE disables the corresponding track, and both disabled is invalid. No
  // codec is opened. Supplied video frames need not match encoder dimensions,
  // pixel format or device; downstream performs any required conversion.
  // Audio layout is borrowed during construction, then independently retained.
  AvEncoderConfig encoder;
  // Optional standby picture, forwarded with its original pixels, dimensions,
  // format and storage. If absent, encoder must specify positive dimensions
  // and a software pixel format to generate a CPU black frame. Ignored when
  // encoder.video_pixel_format is NONE.
  std::optional<ffmpeg::Frame> backup_video;
  SyncerOptions options;
};

// One externally serialized sentinel step. The caller retains head unchanged
// until consumed, including while a forward jump produces replacement frames.
struct SyncerStep {
  bool consumed = false;
  std::optional<ffmpeg::Frame> frame;
  // Earliest useful retry in the caller's elapsed monotonic microseconds,
  // including after a returned frame ends a bounded fill batch.
  std::int64_t wake_us = 0;
  // The output was not accepted. Wait for downstream space before retrying;
  // no frame was committed and the input head remains unconsumed.
  bool blocked = false;
};

// Maintains a shared continuous media timeline without owning threads, queues
// or a realtime clock. The caller paces delivery and serializes Step/Reset.
// EOF is consumed without closing either track. Missing input uses frozen or
// backup video and silence after timeout and the shared clock's two-second
// hold. Output headers are independently owned; media buffers remain shared
// read-only, preserving CPU/GPU storage and layout.
class MW_STREAMER_API Syncer final {
 public:
  // Synchronous, nonblocking admission of a read-only output frame. False
  // leaves the track's next PTS unchanged. Must not reenter Syncer; exceptions
  // propagate to the caller. Empty means the caller accepts the returned frame.
  using OnOutput = std::function<bool(const ffmpeg::Frame&)>;

  // Requires at least one enabled track. Invalid config throws synchronously.
  // Audio layout and backup frame references are independently retained.
  explicit Syncer(SyncerConfig config);
  ~Syncer();
  Syncer(const Syncer&) = delete;
  Syncer& operator=(const Syncer&) = delete;
  Syncer(Syncer&&) = delete;
  Syncer& operator=(Syncer&&) = delete;

  // nullptr means temporarily unavailable input; kNull is not an input event.
  // Produces at most one frame. Disabled tracks, invalid input and arithmetic
  // errors throw to the caller. now_us is elapsed monotonic time since Reset.
  SyncerStep Step(bool video, const AvDecoderReadResult* head,
                  std::int64_t now_us, const OnOutput& on_output = {});
  // Starts a fresh shared timeline; the caller also resets its clock epoch.
  void Reset();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SYNCER_SYNCER_H_

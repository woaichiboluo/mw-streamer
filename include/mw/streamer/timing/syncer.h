#ifndef MW_STREAMER_TIMING_SYNCER_H_
#define MW_STREAMER_TIMING_SYNCER_H_

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/encoder/av_encoder_config.h"

namespace mw::streamer {

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
  std::size_t input_capacity = 63;
  std::chrono::microseconds negative_time_tolerance{250000};
  std::optional<std::chrono::microseconds> negative_time_discard;
  std::chrono::microseconds discontinuity_threshold{1000000};
  std::chrono::microseconds jitter_margin{0};
  std::chrono::microseconds timeout{1000000};
  std::chrono::microseconds freeze{5000000};
  std::chrono::microseconds max_streams_diff{1000};
  std::chrono::microseconds start_ts{10000000};
  // False seeds each track's expected PTS with start_ts, filling positive
  // initial gaps and correcting negative offsets.
  // True leaves it unset until the first frame or input timeout.
  bool forward_start_shift = false;
};

// Paces one live input against a shared clock and maintains continuous audio
// and video output. Owns bounded per-track queues and one scheduling thread.
// Upstream performs video FPS conversion, audio format conversion and business
// filtering; downstream performs any frame-size reblocking for encoding.
// EOF is consumed, not delivered: missing input continues as frozen/backup
// video and silence. Shared media clock holds for two seconds without real
// frames, then extrapolates; backup delivery may be bursty.
// Frames share read-only media buffers, with independently owned headers.
// Video dispatch changes timing only, preserving CPU/GPU storage and layout
// for both user frames and standby pictures; downstream handles adaptation.
class MW_STREAMER_API Syncer final {
 public:
  using OnVideoFrame = std::function<void(ffmpeg::Frame)>;
  using OnAudioFrame = std::function<void(ffmpeg::Frame)>;
  using OnError = std::function<void(std::exception_ptr)>;

  // Requires at least one enabled track. Invalid config throws synchronously.
  // Target config/layout and backup references are retained independently.
  explicit Syncer(SyncerConfig config);
  ~Syncer();
  Syncer(const Syncer&) = delete;
  Syncer& operator=(const Syncer&) = delete;
  Syncer(Syncer&&) = delete;
  Syncer& operator=(Syncer&&) = delete;

  // Configure only while stopped, with lifecycle operations serialized.
  void SetOnVideoFrame(OnVideoFrame callback);
  void SetOnAudioFrame(OnAudioFrame callback);
  void SetOnError(OnError callback);
  // False if already started or thread creation fails. Restart after Stop
  // begins a fresh session, including timing, generations and queued frames.
  bool Start();
  // One producer per track may run concurrently. Feed tracks from independent
  // producer paths: blocking on one queue must not prevent feeding the other.
  // Blocks on a full queue; false means not started/stopped/failed.
  // Invalid frames, disabled tracks,
  // decreasing generations or same-generation input after EOF throw. Across
  // both tracks, submitted generations must be nondecreasing, as in Filter.
  bool PushVideo(const AvDecoderReadResult& input);
  bool PushAudio(const AvDecoderReadResult& input);

  // Both frame callbacks run serially on the scheduling thread, outside queue
  // locks. They must return promptly and must not call Push, Start, Stop or
  // destroy Syncer. RequestStop and error() are safe in callbacks. Exceptions
  // from a frame callback terminate the session and notify OnError once;
  // exceptions from OnError are logged. OnError runs on the same worker and
  // must also not call Push, Start, Stop or destroy Syncer. No callbacks occur
  // after Stop returns.
  std::exception_ptr error() const;
  void RequestStop() noexcept;
  // Wakes blocked producers and scheduler, joins, then discards queued frames.
  // Does not cancel an executing callback. Serialize Start/Stop/destruction;
  // finish external producer calls before destruction or restart.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_TIMING_SYNCER_H_

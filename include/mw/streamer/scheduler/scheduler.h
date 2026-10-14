#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/input/av_media_input.h"
#include "mw/streamer/media_frame.h"

namespace mw::streamer {

enum class SchedulerEndPolicy { kAnyInput, kAllInputs };

struct SchedulerConfig {
  // All Live inputs must use this target frame rate and audio format.
  // Conversion remains in Input; Scheduler neither resizes nor resamples.
  AvEncoderConfig output;
  SchedulerEndPolicy end_policy = SchedulerEndPolicy::kAllInputs;
};

enum class SchedulerInputState {
  kWaiting,
  kRunning,
  kUnderflow,
  kEof,
  kFailed,
  kStopped,
};

struct SchedulerInputFrame {
  // Vector position is the stable input identity, including terminal slots.
  // Underflow/terminal slots retain the last picture with silent audio.
  // A track that has never produced a frame remains empty. A real short audio
  // tail holds its last sample; Input's single-track EOF behavior is retained.
  MediaFrame frame;
  SchedulerInputState state = SchedulerInputState::kWaiting;
  std::uint64_t generation = 0;
  std::uint64_t frame_index = 0;
  std::int64_t pts_us = AV_NOPTS_VALUE;
};

struct SchedulerTick {
  // Scheduling identity, not encoding PTS. A short pipeline returns this
  // tick's picture and audio together on the following channel iteration.
  std::uint64_t frame_number = 0;
  int audio_samples = 0;  // Per channel, common to all prepared audio frames.
};

enum class SchedulerState { kStopped, kRunning, kEnded, kFailed };

struct SchedulerStatus {
  SchedulerState state = SchedulerState::kStopped;
  std::exception_ptr error;
};

// pull all inputs, submit video, process audio,
// consume the previous tick, then pace output. One persistent video worker
// overlaps video processing with the channel worker's audio processing.
// At most two ticks are in flight while awaiting the previous video result.
//
// All callbacks must not throw. Inputs are read-only, including their shared
// media buffers. The audio callback returns the prepared format/sample count.
// Each callback is serial, but video and audio callbacks may run concurrently;
// shared user filter state must therefore be synchronized by the caller.
// Scheduler never flushes user filter caches or generates encoding PTS.
class MW_STREAMER_API Scheduler final {
 public:
  using Inputs = std::vector<SchedulerInputFrame>;
  // Default: Ref() the first nonempty corresponding track in input order.
  // An empty AVFrame (no data[0]) represents no corresponding output track.
  using OnVideo =
      std::function<ffmpeg::Frame(const Inputs&, const SchedulerTick&)>;
  using OnAudio =
      std::function<ffmpeg::Frame(const Inputs&, const SchedulerTick&)>;
  using OnOutput = std::function<void(const MediaFrame&)>;
  // Normal channel EOF only, after every submitted callback and output has
  // finished. User filters may release resources here; no tail is requested.
  using OnEof = std::function<void()>;
  // Consumer notification, after OnEof. Does not mean a consumer has drained
  // its own encoders, queues, files or network outputs.
  using OnEnded = std::function<void()>;
  // Whole-channel failure, after accepted tasks have finished. kAllInputs
  // reports failure only once all inputs terminate, preserving the first
  // input exception. Cancellation does not invoke EOF/end/failure callbacks.
  using OnFailed = std::function<void(std::exception_ptr)>;

  explicit Scheduler(SchedulerConfig config = {});
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Configure callbacks before Start or after Stop. Empty video/audio callbacks
  // restore their defaults. Output/EOF/end/failure callbacks use the channel
  // worker; video callbacks use the video worker. Query/RequestStop are allowed
  // in callbacks; Start/Stop/configuration/destruction are forbidden there.
  void SetOnVideo(OnVideo callback);
  void SetOnAudio(OnAudio callback);
  void SetOnOutput(OnOutput callback);
  void SetOnEof(OnEof callback);
  void SetOnEnded(OnEnded callback);
  void SetOnFailed(OnFailed callback);

  // Retains a fixed nonempty set of distinct, externally started Live inputs.
  // Scheduler is their sole frame reader. Start/Stop/destruction are externally
  // serialized; Stop and finish old reads before restarting any bound Input.
  // Input generations/loop EOF do not reset the channel cadence or clock.
  // Cached loop boundaries are skipped within the same tick; audio carry is
  // retained across drained generations, and cleared on real underflow or
  // discontinuities without a preceding source EOF.
  void Start(std::vector<std::shared_ptr<AvMediaInput>> inputs);
  // Terminal state is published before terminal notification callbacks return.
  // Stop joins those callbacks before their captured resources may be released.
  SchedulerStatus status() const;
  void RequestStop() noexcept;
  // Cancels pacing, joins both workers and releases all input references.
  // Does not stop Input, interrupt user callbacks, or report a normal EOF.
  // A natural Ended/Failed status remains available after Stop.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_H_

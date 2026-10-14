#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/input/av_media_input.h"
#include "mw/streamer/media_frame.h"
#include "mw/streamer/output/output.h"

namespace mw::streamer {

struct SchedulerConfig {
  // All Live inputs must use this target frame rate and audio format.
  // Width/height must be positive and the software pixel format explicit.
  // Construction validates and fixes the complete output format. Conversion
  // remains in Input; Scheduler neither resizes nor resamples.
  AvEncoderConfig output;
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

enum class SchedulerState { kStopped, kRunning };

struct SchedulerStatus {
  SchedulerState state = SchedulerState::kStopped;
};

// pull all inputs, submit video, process audio,
// consume the previous tick, then pace output. One persistent video worker
// overlaps video processing with the channel worker's audio processing.
// At most two ticks are in flight while awaiting the previous video result.
//
// Callbacks must not throw. Inputs, including shared media buffers, are
// read-only. The audio callback returns the prepared format/sample count.
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
  explicit Scheduler(SchedulerConfig config = {});
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Configure callbacks before Start or after Stop. Empty video/audio callbacks
  // restore their defaults. Output callbacks use the channel worker; video
  // callbacks use the video worker. Query/RequestStop are allowed
  // in callbacks; Start/Stop/configuration/destruction are forbidden there.
  void SetOnVideo(OnVideo callback);
  void SetOnAudio(OnAudio callback);
  void SetOnOutput(OnOutput callback);
  // Optional fixed-set fan-out, exclusively driven by this Scheduler. Start
  // initializes frame sinks once. Stop cancels only admission waits; frame
  // sinks remain alive until their owners release them. A healthy clocked frame
  // sink replaces system pacing. Bind before Start or after Stop.
  void SetOutput(std::shared_ptr<Output> output);

  // Retains distinct Live inputs, including an empty set. Scheduler is their
  // sole frame reader. Start/Stop/destruction are externally serialized.
  // Input EOF/failure never ends the channel. Underflow retains the last
  // picture with silent audio. All-failed/empty inputs bypass filters and
  // produce a CPU zero-filled picture and silence in the fixed output format.
  // Loop boundaries do not reset channel cadence or the channel clock.
  // Returns true after all frame sinks initialize and both workers enter their
  // threads. Failure logs and returns false without starting frame processing.
  // Stop is required before restarting a running/finished worker session.
  [[nodiscard]] bool Start(
      std::vector<std::shared_ptr<AvMediaInput>> inputs) noexcept;
  SchedulerStatus status() const;
  void RequestStop() noexcept;
  // Cancels pacing/admission waits and joins workers before releasing inputs.
  // Does not stop Input or Output, nor interrupt user callbacks.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_H_

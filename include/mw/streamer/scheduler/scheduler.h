#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_H_

#include <exception>
#include <functional>
#include <memory>

#include "mw/export.h"
#include "mw/streamer/filter/av_media_filter.h"
#include "mw/streamer/input/av_media_source.h"
#include "mw/streamer/scheduler/realtime_timing.h"
#include "mw/streamer/syncer/syncer.h"

namespace mw::streamer {

namespace internal {
class WriteSignal;
}

struct SchedulerConfig {
  SyncerConfig syncer;
  RealtimeConfig realtime;
  // Audio business callbacks receive target-format frames of this size.
  // Zero retains variable sizes; positive sizes pad boundary tails.
  int output_samples_per_frame = 1024;
};

// One realtime driver releases source frames to independent video/audio
// processing workers using a shared PTS-to-steady-clock mapping. The driver
// owns the passive Syncer and serializes its output without a second FPS/sample
// clock. Real input follows the realtime mapping; sentinel fillers follow its
// bounded-batch deadlines. A shared signal for input, worker completion and
// downstream events wakes the driver. Each track has at most one processing
// task, one converted result and one lookahead source item. Conversion is
// drained before requesting more input. Missing input and normal EOF leave the
// live output running. Disabled input tracks are drained in bounded batches on
// queue readiness, without filtering.
//
// Retains the externally managed source, without starting, stopping or changing
// its business callbacks. The single-consumer queue signal is installed on
// Start and detached on Stop, without closing source queues. This must be its
// only frame consumer while started. Stop Scheduler before restarting the
// source. Serialize configuration, Start/Stop and destruction;
// RequestStop/error are safe from any thread.
class MW_STREAMER_API Scheduler final {
 public:
  using OnVideoFrame = std::function<bool(const ffmpeg::Frame&)>;
  using OnAudioFrame = std::function<bool(const ffmpeg::Frame&)>;
  using OnError = std::function<void(std::exception_ptr)>;

  // Null source or invalid target configuration throws synchronously.
  // Target channel layout and standby frame references are retained separately.
  Scheduler(std::shared_ptr<AvMediaSource> source, SchedulerConfig config);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Configure business callbacks only while Scheduler is stopped. Scheduler
  // owns this filter's lifecycle; callers must not Start/Stop/process it.
  // Audio and video business callbacks may run concurrently.
  AvMediaFilter& media_filter();
  // Configure only while stopped. Output callbacks run serially on the driver's
  // thread and must return promptly. True accepts the read-only frame; false
  // retains the input and next output PTS, suspending only that track until
  // this track's write signal or a 20 ms retry deadline, whichever comes first.
  // Ref() retains accepted storage. Missing callbacks accept
  // and discard output. Unrecoverable failures notify OnError
  // once, on the failing worker (or the lifecycle caller for Start/Stop
  // errors). All callbacks may query error() or RequestStop(), but must not
  // Start/Stop, reconfigure or destroy Scheduler. OnError exceptions are
  // logged.
  void SetOnVideoFrame(OnVideoFrame callback);
  void SetOnAudioFrame(OnAudioFrame callback);
  void SetOnError(OnError callback);

  // Starts internal components only. False if already started or startup
  // fails; startup failures are retained in error() and notified via OnError.
  // Call Stop before restart, including after a worker failure.
  bool Start();
  std::exception_ptr error() const;
  // Bind the downstream queue to this track's capacity signal. A capacity
  // change wakes the shared driver without advancing the other track's
  // revision. Notifications preceding rejection are retained. Does not own
  // or close the downstream queue.
  std::shared_ptr<internal::WriteSignal> output_signal(bool video) const;
  // Cancels internal work without joining or stopping the external source.
  void RequestStop() noexcept;
  // Joins internal workers and releases pending frames. Waits for executing
  // callbacks; no callbacks after return. Idempotent; retains error until
  // Start.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_H_

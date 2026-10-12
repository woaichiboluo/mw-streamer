#ifndef MW_STREAMER_SCHEDULER_SCHEDULER_H_
#define MW_STREAMER_SCHEDULER_SCHEDULER_H_

#include <functional>
#include <memory>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/input/input_frame_source.h"
#include "mw/streamer/media_frame.h"

namespace mw::streamer {

struct MW_STREAMER_API SchedulerConfig {
  AVRational video_frame_rate{30, 1};
};

// Pulls native decoded media through FFmpeg bwdif/fps and asynchronous audio
// resampling, then delivers one MediaFrame per channel tick. Its picture and
// corresponding audio samples are consumed together. Frame callbacks are serial
// on the channel worker. Configure callbacks before Start; serialize Start/Stop
// and never join from callbacks.
class MW_STREAMER_API Scheduler final {
 public:
  using OnReady = std::function<void(const std::vector<ffmpeg::StreamInfo>&)>;
  // Video retains CPU/CUDA buffers. Audio is packed S32 at 48000 Hz and
  // source channel layout. Output time is video ticks/cumulative audio samples.
  // Source audio EOF retains its last block while video continues.
  // Temporary producer underflow repeats the picture with silence.
  // Initial/loop resets with fewer than four queued frames peek the first
  // picture with silence without consuming it.
  using OnFrame = std::function<void(const MediaFrame&)>;
  using OnEnded = std::function<void()>;
  using OnError = std::function<void(int, std::string_view)>;

  explicit Scheduler(SchedulerConfig config = {});
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Once on the filter producer after graph negotiation, before frame delivery.
  // CUDA stream descriptions contain the surface's software pixel layout.
  void SetOnReady(OnReady callback) noexcept;
  // One callback contains both tracks for the tick. Performance callback time
  // for each present track measures this shared boundary, not exclusive work.
  void SetOnFrame(OnFrame callback) noexcept;
  void SetOnEnded(OnEnded callback) noexcept;
  // Fatal source/filter/callback errors request workers to stop. The external
  // pipeline calls Stop; OnError runs outside locks and must not call Stop.
  void SetOnError(OnError callback) noexcept;

  // Input has already selected the required tracks. The source must contain
  // at most one video and one audio track, and at least one of those types.
  bool Start(const DecodedInputSource& source) noexcept;
  // Reopening queues another segment. Old filters and bundles drain first.
  // source.flush instead discards pending old segments and frames for a seek;
  // channel time stays continuous. Rejected after Drain/Stop.
  bool QueueSource(const DecodedInputSource& source) noexcept;
  // No more segments. Drain native filters/bundles and call OnEnded once.
  void Drain() noexcept;
  // Wake workers without joining; request before stopping upstream Input.
  void RequestStop() noexcept;
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_SCHEDULER_H_

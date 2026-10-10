#ifndef MW_STREAMER_PIPELINE_BATCH_PIPELINE_H_
#define MW_STREAMER_PIPELINE_BATCH_PIPELINE_H_

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/pipeline/pipeline_state.h"
#include "mw/streamer/processor/batch_processor.h"

namespace mw::streamer {

struct MW_STREAMER_API BatchPipelineConfig {
  BatchPipelineConfig();

  // Defaults to kBatch at 8x delivery speed. Other modes are rejected; Input's
  // existing playback_speed range, decoder selection and options apply.
  FfmpegInputConfig input;
};

struct BatchPipelineReadyInfo {
  // Original input track descriptions, retained after stopping. Each decoded
  // Frame supplies its actual format, sample_rate and nanosecond time_base.
  std::vector<ffmpeg::StreamInfo> streams;
};

// One independent Input(kBatch) -> BatchProcessor session, without Scheduler.
// Requires Init before Start and destruction before Shutdown. Configure
// callbacks before Start. OnReady/frame callbacks run synchronously on the
// input worker; OnEnded/OnStop run after Input has been joined. Processor
// callbacks must consume their own exceptions, as required by BatchProcessor's
// noexcept contract. State callback exceptions are caught and logged. Callbacks
// may query snapshots but must not call lifecycle/configuration methods or
// destroy this object.
class MW_STREAMER_API BatchPipeline final {
 public:
  using OnReady = BatchProcessor::OnReady;
  using OnVideo = BatchProcessor::OnVideo;
  using OnAudio = BatchProcessor::OnAudio;
  using OnEnded = BatchProcessor::OnEnded;
  using OnStop = BatchProcessor::OnStop;
  using OnStateChanged = std::function<void(const PipelineStatus&)>;

  // Default decoding uses CPU. An external video device is retained by
  // reference for decoding; OnReady borrows the pipeline's device only for its
  // callback.
  explicit BatchPipeline(BatchPipelineConfig config = {});
  explicit BatchPipeline(const ffmpeg::HwDeviceContext& video_device,
                         BatchPipelineConfig config = {});
  ~BatchPipeline();
  BatchPipeline(const BatchPipeline&) = delete;
  BatchPipeline& operator=(const BatchPipeline&) = delete;
  BatchPipeline(BatchPipeline&&) = delete;
  BatchPipeline& operator=(BatchPipeline&&) = delete;

  void SetOnReady(OnReady callback);
  void SetOnVideo(OnVideo callback);
  void SetOnAudio(OnAudio callback);
  void SetOnEnded(OnEnded callback);
  void SetOnStop(OnStop callback);
  void SetOnStateChanged(OnStateChanged callback);

  // Waits for input/processor preparation and the first frame consumer to
  // return. A prepared input with no decoded frames can succeed at normal EOF.
  // Short inputs may already have ended on return. Startup failures finish
  // cleanup before throwing; Input's opening/retry policy applies. Serialize
  // Start calls; Stop from another thread may end the startup wait.
  void Start(std::string_view url);
  // Joins Input, waiting for its current consumer. Normal EOF then calls End
  // followed by Stop; explicit stop or failure calls only Stop. No callbacks
  // run after return. Repeated Stop preserves an already-ended/failed state. A
  // new Start resets the session and retains callback configuration.
  void Stop();

  PipelineState state() const noexcept;
  PipelineStatus status() const;
  BatchPipelineReadyInfo ready_info() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_BATCH_PIPELINE_H_

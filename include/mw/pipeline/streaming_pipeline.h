#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_STREAMING_PIPELINE_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_STREAMING_PIPELINE_H_

#include <functional>
#include <memory>
#include <string>

#include "mw/performance/snapshot.h"
#include "mw/pipeline/config.h"
#include "mw/processor/processor.h"

namespace mw::streamer::pipeline {

enum class StreamingPipelineStatus {
  kIdle,
  kStarting,
  kRunning,
  kFailed,
  // Natural end reports kStopped when media processing completes. The external
  // owner must still call Stop to join workers and release retained resources.
  kStopped,
};

class StreamingPipeline final {
 public:
  using OnStatus = std::function<void(StreamingPipelineStatus status)>;

  explicit StreamingPipeline(StreamingPipelineConfig config);
  ~StreamingPipeline();

  StreamingPipeline(const StreamingPipeline&) = delete;
  StreamingPipeline& operator=(const StreamingPipeline&) = delete;

  // Control methods belong to one external owner thread and must not be called
  // concurrently. Processor and status callbacks must not call control methods
  // or destroy the Pipeline. Audio and video callbacks may run concurrently on
  // their dedicated worker threads. Status callbacks run synchronously on the
  // thread that performs the transition and should return promptly.
  void SetProcessorCallbacks(
      const MwStreamerStreamingProcessorCallbacks& callbacks);
  void SetOnStatus(OnStatus callback);

  // Start is asynchronous. kRunning is reported after every present encoder
  // and, when configured, the shared OutputSession has opened. Without output
  // targets, encoded packets are discarded. Runtime failures transition to
  // kFailed and stop data production without rebuilding the chain. The
  // external owner must then call Stop before destroying or recreating it.
  void Start();
  // Before Start, this replaces the initial Processor configuration delivered
  // through on_start. While starting or running, an initialized Processor
  // receives the update through update_config.
  void UpdateProcessorConfig(std::string config);
  void Stop() noexcept;

  StreamingPipelineStatus status() const noexcept;
  // Returns interval stage statistics since the previous collection together
  // with current-connection network byte counters. Concurrent calls are
  // serialized internally, and every call advances the collection window.
  performance::StreamingPipelineSnapshot CollectPerformance();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_STREAMING_PIPELINE_H_

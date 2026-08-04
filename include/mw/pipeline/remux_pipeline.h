#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_REMUX_PIPELINE_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_REMUX_PIPELINE_H_

#include <functional>
#include <memory>

#include "mw/performance/snapshot.h"
#include "mw/pipeline/config.h"

namespace mw::streamer::pipeline {

enum class RemuxPipelineStatus {
  kIdle,
  kStarting,
  kRunning,
  kFailed,
  // Natural input end reports kStopped after queued packets are written and
  // recording files are finalized. The external owner must still call Stop.
  kStopped,
};

// Pulls compressed media and remuxes it directly to recording or network
// targets without decoding, processing, or re-encoding.
class RemuxPipeline final {
 public:
  using OnStatus = std::function<void(RemuxPipelineStatus status)>;

  explicit RemuxPipeline(RemuxPipelineConfig config);
  ~RemuxPipeline();

  RemuxPipeline(const RemuxPipeline&) = delete;
  RemuxPipeline& operator=(const RemuxPipeline&) = delete;

  // Control methods belong to one external owner thread and must not be called
  // concurrently. Status callbacks must not call control methods or destroy
  // the Pipeline. They run synchronously on the transition thread.
  void SetOnStatus(OnStatus callback);

  // Start is asynchronous. kRunning is reported after source streams and the
  // lifetime OutputSession have opened. Runtime failures stop input production
  // and report kFailed; the external owner must then call Stop.
  void Start();
  void Stop() noexcept;

  RemuxPipelineStatus status() const noexcept;
  // Returns interval packet and media-byte rates together with current queue
  // depth and cumulative network counters. Concurrent calls are serialized,
  // and every call advances the packet-rate collection window.
  performance::RemuxPipelineSnapshot CollectPerformance();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_REMUX_PIPELINE_H_

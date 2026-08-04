#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_FILE_PIPELINE_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_FILE_PIPELINE_H_

#include <functional>
#include <memory>
#include <string>

#include "mw/performance/snapshot.h"
#include "mw/pipeline/config.h"
#include "mw/processor/processor.h"

namespace mw::streamer::pipeline {

enum class FilePipelineStatus {
  kIdle,
  kStarting,
  kRunning,
  kFailed,
  kStopped,
};

// Decodes one local media file at full speed and synchronously delivers frames
// to a File Processor. It does not pace playback, seek, encode, or output
// media.
class FilePipeline final {
 public:
  using OnStatus = std::function<void(FilePipelineStatus status)>;

  explicit FilePipeline(LocalFilePipelineConfig config);
  ~FilePipeline();

  FilePipeline(const FilePipeline&) = delete;
  FilePipeline& operator=(const FilePipeline&) = delete;

  // Control methods belong to one external owner thread and must not be called
  // concurrently. Processor and status callbacks must not call control methods
  // or destroy the Pipeline. Audio and video Processor callbacks run serially
  // on the file worker and synchronously backpressure further file processing.
  void SetProcessorCallbacks(const MwStreamerFileProcessorCallbacks& callbacks);
  void SetOnStatus(OnStatus callback);

  // Start is asynchronous. kRunning is reported after the file, decoders, and
  // Processor have opened. Natural EOF drains decoded frames, emits one
  // kMwStreamerProcessorEndOfInput boundary, and then reports kStopped.
  void Start();
  // Before Start, replaces the initial Processor configuration. Once the
  // Processor is initialized, the update callback may run concurrently with a
  // processing callback, as specified by MwStreamerFileProcessorCallbacks.
  void UpdateProcessorConfig(std::string config);
  // Active Stop interrupts FFmpeg I/O and waits for the current Processor
  // callback. It does not drain or emit an end-of-input boundary.
  void Stop() noexcept;

  FilePipelineStatus status() const noexcept;
  // Stage values cover the interval since the previous call. Position and
  // progress are cumulative and advance only after Processor callbacks return.
  // processing_speed is media-time advancement divided by wall-clock time;
  // 1.0 means real time.
  performance::LocalFilePipelineSnapshot CollectPerformance();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_FILE_PIPELINE_H_

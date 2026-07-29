#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_

#include <memory>
#include <optional>
#include <string>

#include "mw/ffmpeg/frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::processor {

class ProcessorHandler final {
 public:
  // Source information and execution context are copied and remain fixed for
  // the complete Processor lifetime.
  ProcessorHandler(MwStreamerProcessorMode mode,
                   const MwStreamerProcessorSourceInfo& source_info,
                   const MwStreamerExecutionContext& execution);
  ~ProcessorHandler();

  ProcessorHandler(const ProcessorHandler&) = delete;
  ProcessorHandler& operator=(const ProcessorHandler&) = delete;

  void SetCallbacks(const MwStreamerProcessorCallbacks& callbacks);
  // Start copies the initial config string and freezes the output dimensions.
  MwStreamerProcessorStartResult Start(const MwStreamerProcessorConfig& config);

  // Processing and callbacks are synchronous on the calling thread. CUDA
  // processing returns only after the framework stream has completed.
  // kStreaming returns an output frame; kLocalFile returns std::nullopt.
  std::optional<ffmpeg::Frame> ProcessVideo(const ffmpeg::Frame& input);
  std::optional<ffmpeg::Frame> ProcessAudio(const ffmpeg::Frame& input);

  void NotifyBoundary(MwStreamerProcessorBoundaryReason reason);
  // Runtime updates can change only the user-owned opaque string.
  void UpdateConfig(std::string config);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_

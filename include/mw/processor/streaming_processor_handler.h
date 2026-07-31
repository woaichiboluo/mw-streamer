#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_STREAMING_PROCESSOR_HANDLER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_STREAMING_PROCESSOR_HANDLER_H_

#include <memory>

#include "mw/processor/processor_handler.h"

namespace mw::streamer::processor {

class StreamingProcessorHandler final : public ProcessorHandler {
 public:
  StreamingProcessorHandler(const MwStreamerProcessorSourceInfo& source_info,
                            const ffmpeg::HardwareContext* hardware_context);
  ~StreamingProcessorHandler() override;

  StreamingProcessorHandler(const StreamingProcessorHandler&) = delete;
  StreamingProcessorHandler& operator=(const StreamingProcessorHandler&) =
      delete;

  MwStreamerProcessorStartResult Start(
      const MwStreamerStreamingProcessorConfig& config,
      const MwStreamerStreamingProcessorCallbacks& callbacks);

  // Processing and callbacks are synchronous on the calling thread. Hardware
  // processing returns only after the framework execution handle completes.
  ffmpeg::Frame ProcessVideo(const ffmpeg::Frame& input);
  ffmpeg::Frame ProcessAudio(const ffmpeg::Frame& input);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_STREAMING_PROCESSOR_HANDLER_H_

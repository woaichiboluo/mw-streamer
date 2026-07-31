#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_FILE_PROCESSOR_HANDLER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_FILE_PROCESSOR_HANDLER_H_

#include <memory>

#include "mw/processor/processor_handler.h"

namespace mw::streamer::processor {

class FileProcessorHandler final : public ProcessorHandler {
 public:
  FileProcessorHandler(const MwStreamerProcessorSourceInfo& source_info,
                       const ffmpeg::HardwareContext* hardware_context);
  ~FileProcessorHandler() override;

  FileProcessorHandler(const FileProcessorHandler&) = delete;
  FileProcessorHandler& operator=(const FileProcessorHandler&) = delete;

  MwStreamerProcessorStartResult Start(
      const MwStreamerFileProcessorConfig& config,
      const MwStreamerFileProcessorCallbacks& callbacks);

  // File processing consumes decoded frames without allocating media output.
  void ProcessVideo(const ffmpeg::Frame& input);
  void ProcessAudio(const ffmpeg::Frame& input);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_FILE_PROCESSOR_HANDLER_H_

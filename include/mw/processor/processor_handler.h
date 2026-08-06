#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_

#include <memory>
#include <string>

#include "mw/ffmpeg/frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::processor {

class ProcessorHandler {
 public:
  virtual ~ProcessorHandler();

  ProcessorHandler(const ProcessorHandler&) = delete;
  ProcessorHandler& operator=(const ProcessorHandler&) = delete;

  void NotifyBoundary(MwStreamerProcessorBoundaryReason reason);
  void UpdateConfig(std::string config);
  void Stop() noexcept;

 protected:
  ProcessorHandler(const MwStreamerProcessorSourceInfo& source_info,
                   const ffmpeg::HardwareContext* hardware_context);

  void RequireReady(const char* operation) const;
  void RequireStarted(const char* operation) const;
  void MarkStarted(void* user_context,
                   MwStreamerProcessorBoundaryCallback on_boundary,
                   MwStreamerProcessorUpdateConfigCallback update_config,
                   MwStreamerProcessorStopCallback on_stop);
  void ValidateVideoInput(const AVFrame& input,
                          const MwStreamerVideoFrameView& view) const;

  const MwStreamerProcessorSourceInfo& source_info() const noexcept;
  const MwStreamerExecutionContext& execution() const noexcept;
  const ffmpeg::HardwareContext* hardware_context() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_PROCESSOR_HANDLER_H_

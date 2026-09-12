#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_ANALYSIS_PROCESSOR_SINK_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_ANALYSIS_PROCESSOR_SINK_H_

#include <memory>
#include <string>

#include "mw/processor/processor.h"
#include "mw/sink/sink.h"

namespace mw::streamer {

// Synchronously consumes frames through the Analysis Processor C callbacks;
// absent media callbacks ignore that track. Audio/video may execute
// concurrently. Source metadata and the hardware device must stay stable across
// generations. Callbacks borrow their arguments and user_context remains
// borrowed until Stop. No callback may reenter this sink's control, delivery,
// or destruction methods.
class AnalysisProcessorSink final : public Sink {
 public:
  AnalysisProcessorSink(std::string id,
                        MwStreamerAnalysisProcessorCallbacks callbacks);
  ~AnalysisProcessorSink() override;

  AnalysisProcessorSink(const AnalysisProcessorSink&) = delete;
  AnalysisProcessorSink& operator=(const AnalysisProcessorSink&) = delete;

  void OnStreamsReady(const FrameStreamsReady& streams) override;
  void OnAudioFrame(const FrameReady& frame) override;
  void OnVideoFrame(const FrameReady& frame) override;
  void OnTimelineReset(const TimelineReset& reset) override;
  void OnInputEnded(const StreamEnded& end) override;

  // Before startup, stores the value for on_start. Afterwards, updates
  // serialize with each other and invoke on_config_update.
  void UpdateConfig(std::string config);
  // Waits for callbacks; pairs on_stop only with successful startup.
  // Idempotent.
  void Stop() noexcept override;

 protected:
  NodeSnapshot GetOwnPerformance() const override;
  void OnMessage(const SinkMessage& message) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_ANALYSIS_PROCESSOR_SINK_H_

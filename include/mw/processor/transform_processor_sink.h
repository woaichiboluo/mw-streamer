#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_

#include <memory>
#include <string>

#include "mw/processor/config.h"
#include "mw/processor/processor.h"
#include "mw/sink/sink.h"
#include "mw/sink/sink_message.h"

namespace mw::streamer::processor {

// Synchronously transforms frames through Streaming Processor C callbacks and
// fans out in registration order. Missing callbacks pass the original Frame;
// on_start selects a fixed video output size, defaulting to 1920x1080. A
// process callback that mutates its supplied output dimensions causes a
// FatalError to request Pipeline-wide shutdown.
// Callbacks write independent output buffers and finish all writes on return.
// Audio/video may execute concurrently. Source metadata and hardware device
// stay stable across generations; user_context remains borrowed until Stop.
// No callback may reenter control, media delivery, or destruction methods.
// Messages arrive on the owning Pipeline Poller through the on_message hook.
class TransformProcessorSink final : public sink::Sink {
 public:
  TransformProcessorSink(std::string id,
                         processor::StreamingProcessorConfig config,
                         MwStreamerStreamingProcessorCallbacks callbacks);
  ~TransformProcessorSink() override;

  TransformProcessorSink(const TransformProcessorSink&) = delete;
  TransformProcessorSink& operator=(const TransformProcessorSink&) = delete;

  void OnStreamsReady(const media::FrameStreamsReady& streams) override;
  void OnAudioFrame(const media::FrameReady& frame) override;
  void OnVideoFrame(const media::FrameReady& frame) override;
  void OnTimelineReset(const media::TimelineReset& reset) override;
  void OnInputEnded(const media::StreamEnded& end) override;

  // Requires successful startup. Updates serialize with each other, may overlap
  // media callbacks, and cannot overlap Stop or stream boundaries.
  void UpdateConfig(std::string config);
  // Disables message callbacks and waits for in-flight calls before stopping
  // children and invoking C on_stop. Idempotent.
  void Stop() noexcept override;

 protected:
  performance::NodeSnapshot GetOwnPerformance() const override;
  void OnMessage(const sink::SinkMessage& message) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_

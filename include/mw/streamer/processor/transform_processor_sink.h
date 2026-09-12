#ifndef MW_STREAMER_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_
#define MW_STREAMER_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_

#include <memory>
#include <string>

#include "mw/streamer/processor/processor.h"
#include "mw/streamer/sink/sink.h"
#include "mw/streamer/sink/sink_message.h"

namespace mw::streamer {

// Synchronously transforms frames through Transform Processor C callbacks and
// fans out in registration order. Missing callbacks pass the original Frame;
// on_start selects a fixed video output size, defaulting to 1920x1080. A
// process callback that mutates its supplied output dimensions causes a
// FatalError to request Pipeline-wide shutdown.
// Callbacks write independent output buffers and finish all writes on return.
// Audio/video may execute concurrently. Source metadata and hardware device
// stay stable across generations; user_context remains borrowed until Stop.
// No callback may reenter control, media delivery, or destruction methods.
// Messages arrive on the owning Pipeline Poller through the on_message hook.
class TransformProcessorSink final : public Sink {
 public:
  TransformProcessorSink(std::string id,
                         MwStreamerTransformProcessorCallbacks callbacks);
  ~TransformProcessorSink() override;

  TransformProcessorSink(const TransformProcessorSink&) = delete;
  TransformProcessorSink& operator=(const TransformProcessorSink&) = delete;

  void OnStreamsReady(const FrameStreamsReady& streams) override;
  void OnAudioFrame(const FrameReady& frame) override;
  void OnVideoFrame(const FrameReady& frame) override;
  void OnTimelineReset(const TimelineReset& reset) override;
  void OnInputEnded(const StreamEnded& end) override;

  // Before startup, stores the value for on_start. Afterwards, updates
  // serialize with each other and invoke on_config_update.
  void UpdateConfig(std::string config);
  // Disables message callbacks and waits for in-flight calls before stopping
  // children and invoking C on_stop. Idempotent.
  void Stop() noexcept override;

 protected:
  NodeSnapshot GetOwnPerformance() const override;
  void OnMessage(const SinkMessage& message) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PROCESSOR_TRANSFORM_PROCESSOR_SINK_H_

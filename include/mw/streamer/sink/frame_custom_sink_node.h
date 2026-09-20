#ifndef MW_STREAMER_SINK_FRAME_CUSTOM_SINK_NODE_H_
#define MW_STREAMER_SINK_FRAME_CUSTOM_SINK_NODE_H_

#include <memory>
#include <string>

#include "mw/streamer/sink/frame_custom_sink.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

// A decoded-frame terminal supplied by the host through callbacks. It has no
// media downstream.
class FrameCustomSink final : public Sink {
 public:
  FrameCustomSink(std::string id, MwStreamerFrameCustomSinkCallbacks callbacks);
  ~FrameCustomSink() override;

  FrameCustomSink(const FrameCustomSink&) = delete;
  FrameCustomSink& operator=(const FrameCustomSink&) = delete;

  void OnStreamsReady(const FrameStreamsReady& streams) override;
  void OnAudioFrame(const FrameReady& frame) override;
  void OnVideoFrame(const FrameReady& frame) override;
  void OnTimelineReset(const TimelineReset& reset) override;
  void OnInputEnded(const StreamEnded& end) override;
  void Stop() noexcept override;

 protected:
  void OnMessage(const MwStreamerMessage& message) override;
  NodeSnapshot GetOwnPerformance() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_FRAME_CUSTOM_SINK_NODE_H_

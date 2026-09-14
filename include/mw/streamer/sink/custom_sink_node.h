#ifndef MW_STREAMER_SINK_CUSTOM_SINK_NODE_H_
#define MW_STREAMER_SINK_CUSTOM_SINK_NODE_H_

#include <memory>
#include <string>

#include "mw/streamer/sink/custom_sink.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

// A decoded-frame terminal supplied by the host through callbacks. It has no
// media downstream, but may send control messages through the Pipeline's
// message_receiver route.
class CustomSink final : public Sink {
 public:
  CustomSink(std::string id, MwStreamerCustomSinkCallbacks callbacks);
  ~CustomSink() override;

  CustomSink(const CustomSink&) = delete;
  CustomSink& operator=(const CustomSink&) = delete;

  void OnStreamsReady(const FrameStreamsReady& streams) override;
  void OnAudioFrame(const FrameReady& frame) override;
  void OnVideoFrame(const FrameReady& frame) override;
  void OnTimelineReset(const TimelineReset& reset) override;
  void OnInputEnded(const StreamEnded& end) override;
  void Stop() noexcept override;

 protected:
  NodeSnapshot GetOwnPerformance() const override;

 private:
  static void SendFromCallback(
      void* context, const char* type, const void* payload, size_t payload_size,
      const MwStreamerMediaTimestamp* timestamp) noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_CUSTOM_SINK_NODE_H_

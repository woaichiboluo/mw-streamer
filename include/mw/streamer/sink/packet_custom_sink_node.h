#ifndef MW_STREAMER_SINK_PACKET_CUSTOM_SINK_NODE_H_
#define MW_STREAMER_SINK_PACKET_CUSTOM_SINK_NODE_H_

#include <memory>
#include <string>

#include "mw/streamer/sink/packet_custom_sink.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

// Synchronously consumes compressed audio/video from an Input or Encoder.
// Owns no media queue/thread and has no downstream. Supports one audio and one
// video track. Media callback failures report fatal errors without throwing
// into the producer. Stop waits for in-flight media and message callbacks.
class PacketCustomSink final : public Sink {
 public:
  PacketCustomSink(std::string id,
                   MwStreamerPacketCustomSinkCallbacks callbacks);
  ~PacketCustomSink() override;

  PacketCustomSink(const PacketCustomSink&) = delete;
  PacketCustomSink& operator=(const PacketCustomSink&) = delete;

  void OnStreamsReady(const StreamsReady& streams) noexcept override;
  void OnPacket(const PacketReady& packet) noexcept override;
  void OnTimelineReset(const TimelineReset& reset) noexcept override;
  void OnInputEnded(const StreamEnded& end) noexcept override;
  void Stop() noexcept override;

 protected:
  void OnMessage(const MwStreamerMessage& message) override;
  NodeSnapshot GetOwnPerformance() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_PACKET_CUSTOM_SINK_NODE_H_

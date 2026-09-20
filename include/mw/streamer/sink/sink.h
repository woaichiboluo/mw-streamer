#ifndef MW_STREAMER_SINK_SINK_H_
#define MW_STREAMER_SINK_SINK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/streamer/media/stream_event.h"
#include "mw/streamer/performance/pipeline_snapshot.h"
#include "mw/streamer/sink/fatal_error.h"
#include "mw/streamer/sink/sink_message.h"

namespace mw::streamer {
class Pipeline;
}

namespace mw::streamer {

enum class SinkMediaType { kNone, kPacket, kFrame };

enum class PacketSinkState {
  kIdle,
  kRunning,
  kDraining,
  kEnded,
  kFailed,
  kStopped,
};

// One node in an exclusively owned media tree. Queues and media execution
// remain concrete-sink responsibilities. Packet calls are serialized; audio
// and video may run concurrently, each ordered, with exclusive stream
// boundaries. Packet consumers must contain exceptions in input callbacks,
// since Input delivery is noexcept; report failures through the fatal route
// when appropriate. Derived destructors must Stop before releasing state
// accessed by workers.
class Sink {
 public:
  using OnFatalError = std::function<void(const std::string&)>;

  explicit Sink(std::string id, SinkMediaType input_type,
                SinkMediaType output_type = SinkMediaType::kNone);
  virtual ~Sink();
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  // Setup only, not concurrent with queries, delivery or Stop. Checks media
  // compatibility and takes unique ownership. Terminal nodes reject children.
  void AddSink(std::unique_ptr<Sink> sink);
  // Immutable, nonempty ID. Pipeline requires uniqueness across the whole tree.
  const std::string& id() const noexcept;
  SinkMediaType input_type() const noexcept;
  SinkMediaType output_type() const noexcept;

  // Setup-only fatal route. Must not throw, block, or invoke control methods.
  void SetOnFatalError(OnFatalError callback);

  // Borrowed arguments; retain references/copies for asynchronous processing.
  // Unsupported media entry points throw. Concrete sinks own event forwarding
  // times: EOF draining, codec readiness and timeline absorption differ by
  // node.
  virtual void OnStreamsReady(const StreamsReady& streams);
  virtual void OnStreamsReady(const FrameStreamsReady& streams);
  virtual void OnPacket(const PacketReady& packet);
  virtual void OnAudioFrame(const FrameReady& frame);
  virtual void OnVideoFrame(const FrameReady& frame);
  virtual void OnTimelineReset(const TimelineReset& reset);
  virtual void OnInputEnded(const StreamEnded& end);

  // First shutdown phase, safe while upstream delivery is in progress. Wake
  // blocked producers and reject further work without joining workers or
  // releasing state. The default forwards the request to children. Pipeline
  // calls this before stopping Input, then calls Stop after Input has joined.
  virtual void RequestStop() noexcept;

  // Outside callbacks/owned workers, after upstream media has stopped.
  // Overrides stop messages before business state is released, join media
  // workers, then StopDownstream before releasing anything borrowed by
  // consumers. Idempotent.
  virtual void Stop() noexcept;

  // Nondestructive owned tree, safe during media execution/Stop, not concurrent
  // registration or destruction. Concrete nodes provide only their own metrics.
  NodeSnapshot GetPerformance() const;

 protected:
  const std::vector<std::unique_ptr<Sink>>& downstream() const noexcept;
  void CloseRegistration() noexcept;
  // Call after business initialization and before notifying children Ready.
  // Idempotent across reconnects; no message queue or thread is owned here.
  void StartMessages();
  void StopMessages() noexcept;
  void StopDownstream() noexcept;

  // Optional receiver hook, serialized on the Pipeline message Poller. It may
  // overlap media calls and may call Pipeline::SendMessage, but must not invoke
  // lifecycle control methods. Default ignores.
  virtual void OnMessage(const MwStreamerMessage& message);

  void SendStreamsReady(const StreamsReady& streams);
  void SendStreamsReady(const FrameStreamsReady& streams);
  void SendPacket(const PacketReady& packet);
  void SendAudioFrame(const FrameReady& frame);
  void SendVideoFrame(const FrameReady& frame);
  void SendTimelineReset(const TimelineReset& reset);
  void SendInputEnded(const StreamEnded& end);

  virtual NodeSnapshot GetOwnPerformance() const;
  // Handles child or message-callback FatalError before propagating upstream.
  virtual void HandleFatalError(const std::string& error) noexcept;
  void ReportFatalError(const std::string& error) noexcept;

 private:
  friend class Pipeline;
  // Called only by the owning Pipeline message loop. Stops wait for this call.
  void DispatchSinkMessage(const MwStreamerMessage& message) noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_SINK_H_

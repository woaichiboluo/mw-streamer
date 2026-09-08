#ifndef MW_STREAMER_INCLUDE_MW_SINK_SINK_H_
#define MW_STREAMER_INCLUDE_MW_SINK_SINK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mw/media/stream_event.h"
#include "mw/performance/pipeline_snapshot.h"
#include "mw/sink/fatal_error.h"
#include "mw/sink/sink_message.h"

namespace mw::streamer::pipeline {
class Pipeline;
}

namespace mw::streamer::sink {

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
  using MessageSender = std::function<void(const SinkMessage&)>;
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

  // Setup only. Pipeline binds the target ID and queue implementation into
  // this function. It may run concurrently on media/message threads, must copy
  // borrowed data before returning, and must enqueue without invoking receiver
  // business inline. Captured resources must outlive this sink's Stop. Empty
  // clears the sender; no implicit binding.
  void SetMessageSender(MessageSender sender);

  // Setup-only fatal route. Must not throw, block, or invoke control methods.
  void SetOnFatalError(OnFatalError callback);

  // Borrowed arguments; retain references/copies for asynchronous processing.
  // Unsupported media entry points throw. Concrete sinks own event forwarding
  // times: EOF draining, codec readiness and timeline absorption differ by
  // node.
  virtual void OnStreamsReady(const media::StreamsReady& streams);
  virtual void OnStreamsReady(const media::FrameStreamsReady& streams);
  virtual void OnPacket(const media::PacketReady& packet);
  virtual void OnAudioFrame(const media::FrameReady& frame);
  virtual void OnVideoFrame(const media::FrameReady& frame);
  virtual void OnTimelineReset(const media::TimelineReset& reset);
  virtual void OnInputEnded(const media::StreamEnded& end);

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
  performance::NodeSnapshot GetPerformance() const;

 protected:
  const std::vector<std::unique_ptr<Sink>>& downstream() const noexcept;
  void CloseRegistration() noexcept;
  // Call after business initialization and before notifying children Ready.
  // Idempotent across reconnects; no message queue or thread is owned here.
  void StartMessages();
  void StopMessages() noexcept;
  void StopDownstream() noexcept;

  // Calls the injected sender. Unbound/stopped sends are ignored. Pipeline
  // copies and queues messages, ignoring them before Start/after Stop; invalid
  // payloads and allocation failures may throw. No delivery acknowledgment.
  void SendMessage(const SinkMessage& message) const;
  // Optional receiver hook, serialized on the Pipeline message Poller. It may
  // overlap media calls and must not invoke control methods. Default ignores.
  virtual void OnMessage(const SinkMessage& message);

  void SendStreamsReady(const media::StreamsReady& streams);
  void SendStreamsReady(const media::FrameStreamsReady& streams);
  void SendPacket(const media::PacketReady& packet);
  void SendAudioFrame(const media::FrameReady& frame);
  void SendVideoFrame(const media::FrameReady& frame);
  void SendTimelineReset(const media::TimelineReset& reset);
  void SendInputEnded(const media::StreamEnded& end);

  virtual performance::NodeSnapshot GetOwnPerformance() const;
  // Handles child or message-callback FatalError before propagating upstream.
  virtual void HandleFatalError(const std::string& error) noexcept;
  void ReportFatalError(const std::string& error) noexcept;

 private:
  friend class pipeline::Pipeline;
  // Called only by the owning Pipeline message loop. Stops wait for this call.
  void DispatchMessage(const SinkMessage& message) noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::sink

#endif  // MW_STREAMER_INCLUDE_MW_SINK_SINK_H_

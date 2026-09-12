#ifndef MW_STREAMER_INPUT_INPUT_H_
#define MW_STREAMER_INPUT_INPUT_H_

#include "mw/streamer/input/input_state.h"
#include "mw/streamer/media/stream_event.h"
#include "mw/streamer/performance/pipeline_snapshot.h"

namespace mw::streamer {

// Owns source acquisition, independently of downstream queues and processing.
// Start, Stop and destruction must run outside the input's execution context;
// they may synchronously wait for that context. state() is safe in callbacks.
class Input {
 public:
  class Observer {
   public:
    virtual ~Observer() = default;

    // Serialized synchronous delivery on the input's execution context. Each
    // argument is borrowed for the call; copy it to retain it in a sink queue.
    // Must not call Input control methods or destroy Input from this callback.
    // Within a generation, streams precede packets and end follows the last
    // packet. A timeline reset precedes replacement streams.
    virtual void OnStreamsReady(
        const StreamsReady& streams) noexcept = 0;
    virtual void OnPacket(const PacketReady& packet) noexcept = 0;
    virtual void OnTimelineReset(
        const TimelineReset& reset) noexcept = 0;
    virtual void OnInputEnded(const StreamEnded& end) noexcept = 0;

    // Source connection/retry/error notification for the coordinating owner.
    // The input state snapshot is updated before this call.
    virtual void OnInputStateChanged(
        const InputStateChanged& state) noexcept = 0;
  };

  virtual ~Input() = default;

  // Starts asynchronously and borrows observer until Stop completes. Each
  // instance starts once. Invalid configuration throws synchronously; runtime
  // source errors arrive as events. If Start throws, observer is not retained.
  // The owner must Stop before destroying the observer, including after EOF
  // or failure.
  virtual void Start(Observer& observer) = 0;

  // Idempotent. Waits for in-flight delivery; no events follow its return.
  // Does not wait for downstream queues to drain.
  virtual void Stop() noexcept = 0;

  // Thread-safe snapshot, not a substitute for ordered stream events.
  virtual InputState state() const noexcept = 0;

  // Nondestructive statistics; safe during acquisition and Stop, but not
  // destruction. Custom inputs may override to expose their own recording.
  virtual NodeSnapshot GetPerformance() const {
    return {{}, "Input", {}, {}};
  }
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_INPUT_H_

#ifndef MW_STREAMER_OUTPUT_OUTPUT_H_
#define MW_STREAMER_OUTPUT_OUTPUT_H_

#include <atomic>
#include <exception>
#include <memory>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/output/frame_sink.h"

namespace mw::streamer {

enum class FrameSinkState { kWaiting, kRunning, kFailed };

struct FrameSinkStatus {
  FrameSinkState state = FrameSinkState::kWaiting;
  std::exception_ptr error;
};

enum class OutputState { kStopped, kRunning };

struct OutputStatus {
  OutputState state = OutputState::kStopped;
  // Constructor order is the stable frame sink identity, including failed
  // slots.
  std::vector<FrameSinkStatus> frame_sinks;
};

// Fixed-set channel fan-out. First submit to all healthy frame sinks, then wait
// for all admission results. Blocking frame sinks backpressure the whole
// channel; dropping frame sinks choose their own policy. Runtime failure
// isolates one frame sink. No frame sinks is valid. Runtime-failed slots remain
// queryable, but release their frame sink reference. Initialization failures
// retain frame sinks for retry. Frame sinks live until their last shared owner
// releases them.
//
// Start/Send/destruction must be externally serialized. Query may run
// concurrently. Do not use one frame sink in two Outputs.
class MW_STREAMER_API Output final {
 public:
  explicit Output(std::vector<std::shared_ptr<FrameSink>> frame_sinks = {});
  ~Output();
  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;
  Output(Output&&) = delete;
  Output& operator=(Output&&) = delete;

  // Initializes frame sinks once with a retained copy of the complete channel
  // format. Reusing the same format is allowed; changing it is rejected.
  // Returns true only when every frame sink is initialized. Initialization
  // failure leaves Output stopped; retrying the same format initializes only
  // frame sinks that have not succeeded. Successfully initialized frame sinks
  // remain alive. Invalid configuration and initialization failure are logged
  // and return false.
  [[nodiscard]] bool Start(const AvEncoderConfig& format) noexcept;
  // Submit all healthy frame sinks before waiting for admission. Cancellation
  // interrupts only this call, leaving frame sinks and outstanding work alive.
  // The next call resolves outstanding work before submitting another frame.
  // Returns false on cancellation, true when all admission results resolve.
  bool Send(const OutputFrame& frame,
            const std::atomic<bool>* cancelled = nullptr);
  bool HasSynchronizationClock() const;
  OutputStatus status() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_OUTPUT_OUTPUT_H_

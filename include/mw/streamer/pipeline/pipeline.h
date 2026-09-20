#ifndef MW_STREAMER_PIPELINE_PIPELINE_H_
#define MW_STREAMER_PIPELINE_PIPELINE_H_

#include <memory>
#include <string>

#include "mw/streamer/input/input.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

enum class PipelineState {
  kIdle,
  kRunning,
  kStopping,
  kStopped,
  kFailed,
};

// Connects one input to exclusively owned sinks. Delivery is synchronous in
// registration order on the input's execution context; queues and processing
// of media belong to each sink. Source status is observed separately from sink
// status. A dedicated control thread waits for fatal reports and stops the
// input and all sinks. It never processes media or polls sink state. One
// exclusive Poller uses its task queue to dispatch messages; callbacks are
// serialized across all sinks in this Pipeline. Start, Stop and destruction
// must run outside the input's execution context. Sink callbacks must not call
// pipeline control methods or destroy it; SendMessage is allowed.
class Pipeline final {
 public:
  // Takes ownership of a non-null, not-yet-started input.
  explicit Pipeline(std::unique_ptr<Input> input);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Takes exclusive ownership. Only allowed before the first Start or Stop.
  // A null sink throws invalid_argument; late registration throws logic_error.
  void AddSink(std::unique_ptr<Sink> sink);

  // Asynchronously delivers to one sink in this Pipeline's tree. Copies all
  // borrowed message data before returning; no delivery acknowledgment.
  // Thread-safe with Start/Stop and callable from sink callbacks. Calls before
  // Start or after shutdown begins are ignored; unready/stopped targets ignore
  // delivery. While submission is open, unknown IDs, null types and invalid
  // payloads throw invalid_argument; allocation failures may throw. Destruction
  // must not race with callers. Message ordering is independent of media
  // delivery.
  void SendMessage(const std::string& target_sink_id,
                   const MwStreamerMessage& message);

  // Stores opaque business configuration for a Processor Sink. Before that
  // Processor starts, its on_start callback receives the latest value; after
  // it starts, on_config_update receives each new value. Every Processor
  // defaults to an empty configuration string. Unknown or non-Processor IDs
  // throw invalid_argument; calls after Stop throw logic_error.
  void SetProcessorConfig(std::string processor_id, std::string config);

  // Requires at least one sink and allows one start attempt per instance.
  // Source validation errors propagate to the caller after stopping input.
  // Runtime input errors are available through input_status().
  void Start();

  // Idempotent. Closes message submission, skips pending message tasks and
  // waits for in-flight callbacks. Requests sinks to release blocked input
  // delivery, stops input, then stops each sink and waits
  // for its execution to finish. Queue cleanup policy belongs to each sink.
  // Sinks remain owned until destruction.
  void Stop() noexcept;

  // A fatal report immediately sets kFailed; that does not mean shutdown has
  // finished. Stop waits for shutdown. Failure and its first cause survive
  // Stop. Source connection errors remain separately available through
  // input_status.
  PipelineState state() const noexcept;
  std::string error() const;

  // Thread-safe snapshot of the latest input state notification, including
  // generation, error and retry information. A media boundary can precede its
  // state notification; sinks must use the reason carried by OnInputEnded.
  // Input EOF does not mean that sinks have finished processing their queues.
  InputStateChanged input_status() const;

  // Returns an owned statistics tree, including every downstream consumer.
  // Reading never clears counters or changes another reader's sampling window.
  // Use Snapshot::Find and WithRatesSince for selection and interval rates.
  // Safe with Start/Stop/media delivery; not AddSink, downstream
  // registration or destruction. Custom nodes must honor their read contract.
  PipelineSnapshot GetPerformance() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_PIPELINE_H_

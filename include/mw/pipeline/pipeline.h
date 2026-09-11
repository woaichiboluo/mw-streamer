#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_H_

#include <memory>
#include <string>

#include "mw/input/input.h"
#include "mw/sink/sink.h"

namespace mw::streamer::pipeline {

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
// pipeline control methods or destroy it.
class Pipeline final {
 public:
  // Takes ownership of a non-null, not-yet-started input.
  explicit Pipeline(std::unique_ptr<input::Input> input);
  ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Takes exclusive ownership. Only allowed before the first Start or Stop.
  // A null sink throws invalid_argument; late registration throws logic_error.
  void AddSink(std::unique_ptr<sink::Sink> sink);

  // Setup only. Both IDs must belong to this Pipeline's sink tree; checked at
  // Start, along with ID uniqueness. Repeated binding replaces the target.
  // No automatic binding. Message routing is independent of media connections.
  void SetMessageReceiver(std::string sender_id, std::string receiver_id);

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
  // Sinks remain owned. Destruction releases input, sinks, then the message
  // facilities, keeping injected sender functions valid through sink teardown.
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
  input::InputStateChanged input_status() const;

  // Returns an owned statistics tree, including every downstream consumer.
  // Reading never clears counters or changes another reader's sampling window.
  // Use Snapshot::Find and WithRatesSince for selection and interval rates.
  // Safe with Start/Stop/media delivery; not AddSink, downstream
  // registration or destruction. Custom nodes must honor their read contract.
  performance::PipelineSnapshot GetPerformance() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_H_

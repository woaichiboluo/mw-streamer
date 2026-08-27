#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_EVENT_MAILBOX_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_EVENT_MAILBOX_H_

#include <cstddef>
#include <memory>

#include "mw/processor/processor.h"

namespace mw::streamer::processor {
class StreamingProcessorHandler;
}

namespace mw::streamer::pipeline::internal::streaming {

enum class OutputEventSubmitResult {
  kAccepted,
  kQueueFull,
  kStopped,
};

// Delivers copied output events to the Processor on one dedicated thread.
// Stop joins that thread; no callback can run after Stop returns.
class OutputEventMailbox final {
 public:
  OutputEventMailbox(std::size_t queue_capacity,
                     processor::StreamingProcessorHandler& processor);
  ~OutputEventMailbox();

  OutputEventMailbox(const OutputEventMailbox&) = delete;
  OutputEventMailbox& operator=(const OutputEventMailbox&) = delete;

  void Start();
  OutputEventSubmitResult Submit(const MwStreamerOutputEvent& event);
  void RequestStop() noexcept;
  void Stop() noexcept;
  std::size_t queue_depth() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_EVENT_MAILBOX_H_

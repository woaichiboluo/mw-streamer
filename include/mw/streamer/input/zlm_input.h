#ifndef MW_STREAMER_INPUT_ZLM_INPUT_H_
#define MW_STREAMER_INPUT_ZLM_INPUT_H_

#include <memory>
#include <string>

#include "mw/streamer/input/config.h"
#include "mw/streamer/input/input.h"
#include "mw/streamer/zlm/config.h"

namespace mw::streamer {

// Adapts PlayerProxy without exposing its callbacks to consumers. Events are
// delivered synchronously on an exclusively extracted pool poller, retained
// for this input's lifetime and never allocated to shared output work. Network
// reconnects retain the observer and introduce a new timeline before
// replacement streams. Start, Stop, and destruction must run outside that
// poller's execution context.
class ZlmInput final : public Input {
 public:
  explicit ZlmInput(ZlmInputConfig config);
  ~ZlmInput() override;

  ZlmInput(const ZlmInput&) = delete;
  ZlmInput& operator=(const ZlmInput&) = delete;

  // Validates configuration synchronously, then starts input asynchronously.
  // Each instance can start once; starting after Stop throws logic_error.
  // Calling on the owner poller throws logic_error before borrowing observer.
  void Start(Observer& observer) override;
  // Waits for in-flight event delivery and releases the borrowed observer.
  // Calling on the owner poller violates the thread contract and terminates.
  // Repeated calls from outside that poller are harmless.
  void Stop() noexcept override;
  InputState state() const noexcept override;
  NodeSnapshot GetPerformance() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_ZLM_INPUT_H_

#ifndef MW_STREAMER_INCLUDE_MW_INPUT_INPUT_STATE_H_
#define MW_STREAMER_INCLUDE_MW_INPUT_INPUT_STATE_H_

#include <cstdint>
#include <string>

namespace mw::streamer {

// Source state only: kEnded does not mean downstream sinks have drained.
enum class InputState {
  kIdle,
  kConnecting,
  kReady,
  kWaitingRetry,
  kEnded,
  kFailed,
  kStopped,
};

struct InputStateChanged {
  std::uint64_t generation;
  InputState state;
  std::string error;
  bool will_retry = false;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_INPUT_INPUT_STATE_H_

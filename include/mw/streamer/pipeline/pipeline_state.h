#ifndef MW_STREAMER_PIPELINE_PIPELINE_STATE_H_
#define MW_STREAMER_PIPELINE_PIPELINE_STATE_H_

#include <string>

namespace mw::streamer {

enum class PipelineState {
  kIdle,
  kStarting,
  kRunning,
  kReconnecting,
  kStopping,
  kStopped,
  kEnded,
  kFailed,
};

// Terminal states are published only after all local workers and outputs have
// finished. kEnded is successful EOF, kStopped is an explicit stop, and kFailed
// is a startup or fatal runtime error. A new Start clears failure details.
struct PipelineStatus {
  PipelineState state = PipelineState::kIdle;
  std::string error_source;
  int error_code = 0;
  std::string error_message;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_PIPELINE_STATE_H_

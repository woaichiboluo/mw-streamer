#ifndef MW_STREAMER_INCLUDE_MW_INIT_INIT_H_
#define MW_STREAMER_INCLUDE_MW_INIT_INIT_H_

#include "mw/log/logging.h"

namespace mw::streamer {

struct InitConfig {
  log::LogConfig log;
};

// Initializes process-wide mw-streamer modules once. The first successful call
// owns the configuration; later calls are no-ops. Streamer logging remains
// available through its lazy default logger before this call.
void Init(const InitConfig& config = {});

// Must be called from the owning application thread after media sessions,
// reconnect tasks, and their poller callbacks have stopped. Reinitialization
// after shutdown is not supported.
void Shutdown() noexcept;

bool IsInitialized() noexcept;

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_INIT_INIT_H_

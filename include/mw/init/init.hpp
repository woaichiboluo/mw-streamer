#pragma once

#include "mw/log/logging.hpp"

namespace mw {

struct InitConfig {
  log::LogConfig log;
};

// Initializes process-wide mw-streamer modules once. The first successful call
// owns the configuration; later calls are no-ops. Streamer logging remains
// available through its lazy default logger before this call.
void init(const InitConfig& config = {});

// Must be called from the owning application thread after media sessions,
// reconnect tasks, and their poller callbacks have stopped. Reinitialization
// after shutdown is not supported.
void shutdown() noexcept;

bool initialized() noexcept;

}  // namespace mw

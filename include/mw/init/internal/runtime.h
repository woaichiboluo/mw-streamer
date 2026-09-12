#ifndef MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_
#define MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_

#include "mw/log/logging.h"
#include "mw/zlm/config.h"

namespace mw::streamer::internal {

struct RuntimeConfig {
  LogConfig log;
  ZlmConfig zlm;
};

// Initializes process-global logging and ZLToolKit resources once. This is an
// implementation detail used by media entry points, never a host operation.
void EnsureInitialized(const RuntimeConfig& config = {});

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_

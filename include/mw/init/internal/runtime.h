#ifndef MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_
#define MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_

#include "mw/log/logging.h"
#include "mw/zlm/config.h"

namespace mw::streamer::init::internal {

struct RuntimeConfig {
  log::LogConfig log;
  zlm::Config zlm;
};

// Initializes process-global logging and ZLToolKit resources once. This is an
// implementation detail used by media entry points, never a host operation.
void EnsureInitialized(const RuntimeConfig& config = {});

}  // namespace mw::streamer::init::internal

#endif  // MW_STREAMER_INCLUDE_MW_INIT_INTERNAL_RUNTIME_H_

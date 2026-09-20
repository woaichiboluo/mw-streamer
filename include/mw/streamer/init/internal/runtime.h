#ifndef MW_STREAMER_INIT_INTERNAL_RUNTIME_H_
#define MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

#include "mw/log.h"
#include "mw/streamer/zlm/config.h"

namespace mw::streamer::internal {

struct RuntimeConfig {
  mw::log::LogConfig log;
  ZlmConfig zlm;
};

// Pipeline is the sole runtime owner: all network inputs and outputs belong
// to its task graph. It owns the runtime until its tasks and resources are gone.
// Builders also hold a temporary lease so failed construction is cleaned up.
// Acquire/release must run outside the runtime's worker threads.
class RuntimeLease final {
 public:
  explicit RuntimeLease(const RuntimeConfig& config = {});
  ~RuntimeLease();
  RuntimeLease(const RuntimeLease&) = delete;
  RuntimeLease& operator=(const RuntimeLease&) = delete;
};

// Initializes logging and ZLToolKit resources for the current runtime. This is an
// implementation detail used by media entry points, never a host operation.
void EnsureInitialized(const RuntimeConfig& config = {});

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

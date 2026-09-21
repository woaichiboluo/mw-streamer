#ifndef MW_STREAMER_INIT_INTERNAL_RUNTIME_H_
#define MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

#include <stdexcept>

#include "mw/log.h"
#include "mw/streamer/zlm/config.h"

namespace mw::streamer::internal {

struct RuntimeConfig {
  mw::log::LogConfig log;
  ZlmConfig zlm;
};

// Initializes process-global logging and ZLToolKit resources once. This is an
// implementation detail used by media entry points, never a host operation.
void EnsureInitialized(const RuntimeConfig& config = {});

class RuntimeStateError : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

// Prevents terminal shutdown during construction and throughout Pipeline
// ownership. Releasing the last use never shuts down the runtime implicitly.
class RuntimeUse final {
 public:
  explicit RuntimeUse(const RuntimeConfig& config = {});
  ~RuntimeUse();
  RuntimeUse(const RuntimeUse&) = delete;
  RuntimeUse& operator=(const RuntimeUse&) = delete;
};

void ShutdownRuntime();

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

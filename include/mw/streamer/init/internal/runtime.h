#ifndef MW_STREAMER_INIT_INTERNAL_RUNTIME_H_
#define MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

#include <stdexcept>

#include "mw/streamer/api.h"

namespace mw::streamer::internal {

class RuntimeStateError : public std::logic_error {
 public:
  using std::logic_error::logic_error;
};

void InitializeRuntime(const MwLogConfig* log_config,
                       const MwZlmConfig* zlm_config);
bool IsRuntimeInitialized() noexcept;

// Prevents terminal shutdown during construction and throughout ownership.
// Construction requires an explicitly initialized runtime. Releasing the last
// use never shuts down the runtime implicitly.
class RuntimeUse final {
 public:
  RuntimeUse();
  ~RuntimeUse();
  RuntimeUse(const RuntimeUse&) = delete;
  RuntimeUse& operator=(const RuntimeUse&) = delete;
};

void ShutdownRuntime();

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INIT_INTERNAL_RUNTIME_H_

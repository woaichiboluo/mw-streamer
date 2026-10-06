#ifndef MW_STREAMER_INIT_INIT_H_
#define MW_STREAMER_INIT_INIT_H_

#include <cstddef>

#include "mw/export.h"
#include "mw/log.h"

namespace mw::streamer {

class MwStreamerContext;

struct MW_STREAMER_API InitConfig {
  InitConfig();

  MwLogConfig log;
  // Zero selects the hardware concurrency reported by ZLM.
  std::size_t event_poller_threads = 0;
  std::size_t work_threads = 0;
  bool enable_cpu_affinity = true;
};

// Creates the process-wide runtime. The caller must call Init() only once
// and before using Inputs.
// Initializes logging, FFmpeg and ZLM networking, and both ZLM pools. The
// timestamp thread starts on the first request for time. Initialization
// failures throw and release all resources acquired by this call.
MW_STREAMER_API MwStreamerContext* Init(const InitConfig& config = {});

// Call after destroying all Inputs and other ZLM users, before unloading the
// library, outside Input callbacks. The caller guarantees this ordering and
// passes the context returned by Init() exactly once. The context is destroyed;
// do not delete or reuse its pointer. A null context is a no-op.
MW_STREAMER_API void Shutdown(MwStreamerContext* context);

}  // namespace mw::streamer

#endif  // MW_STREAMER_INIT_INIT_H_

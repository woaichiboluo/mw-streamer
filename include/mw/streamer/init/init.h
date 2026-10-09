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

// Creates the process-wide runtime before using any ZLM-backed objects.
// Only one context may be active; another Init() is allowed after Shutdown().
// Initializes logging and FFmpeg, then delegates ZLM's owned resources to its
// init/shutdown lifecycle. Requests 1 ms timer resolution on Windows until
// Shutdown(). On POSIX, ignores SIGPIPE process-wide before initializing media
// runtimes; this remains in effect after Shutdown() or a later initialization
// failure. Initialization failures release resources acquired by this call
// and propagate the exception.
MW_STREAMER_API MwStreamerContext* Init(const InitConfig& config = {});

// Call after destroying all Inputs, Schedulers, Processors and other
// ZLM users, before unloading the
// library, outside Input callbacks. The caller guarantees this ordering and
// passes the context returned by Init() exactly once. The context is destroyed;
// do not delete or reuse its pointer. A null context is a no-op.
MW_STREAMER_API void Shutdown(MwStreamerContext* context);

}  // namespace mw::streamer

#endif  // MW_STREAMER_INIT_INIT_H_

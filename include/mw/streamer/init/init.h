#ifndef MW_STREAMER_INIT_INIT_H_
#define MW_STREAMER_INIT_INIT_H_

#include <cstddef>
#include <string>

#include "mw/export.h"
#include "mw/log.h"
#include "mw/streamer/performance/config.h"

namespace mw::streamer {

class MwStreamerContext;

struct TlsConfig {
  // PEM/P12 file containing the server certificate chain and private key.
  // Empty means no TLS server certificate. Credentials belong to this runtime.
  std::string certificate_file;
  std::string certificate_password;
  // Optional additional trusted certificate/CA file for ZLM TLS clients.
  // Default OpenSSL trust paths are retained. FFmpeg's TLS clients are
  // separate.
  std::string trusted_certificate_file;
};

struct MW_STREAMER_API InitConfig {
  InitConfig();

  mw::log::LogConfig log;
  PerformanceConfig performance;
  // Zero selects the hardware concurrency reported by ZLM.
  std::size_t event_poller_threads = 2;
  std::size_t work_threads = 1;
  bool enable_cpu_affinity = true;
  TlsConfig tls;
};

// Creates the process-wide runtime before using any ZLM-backed objects.
// Only one context may be active; another Init() is allowed after Shutdown().
// On Windows, sets an attached console's input/output code pages to UTF-8
// before initializing logging; the code pages remain set after Shutdown().
// Copies the fixed performance configuration, independently of log.modules.
// Initializes logging and FFmpeg, then delegates ZLM's owned resources to its
// init/shutdown lifecycle. Requests 1 ms timer resolution on Windows until
// Shutdown(). On POSIX, ignores SIGPIPE process-wide before initializing media
// runtimes; this remains in effect after Shutdown() or a later initialization
// failure. Initialization failures release resources acquired by this call
// and propagate the exception.
// Owns FFmpeg's process-wide logging callback while active, forwarding to the
// "ffmpeg" log module. Do not replace that callback during the runtime.
MW_STREAMER_API MwStreamerContext* Init(const InitConfig& config = {});

// Call after destroying all media objects and other ZLM users, before
// unloading the library, outside Input callbacks. The caller guarantees this
// ordering and passes the context returned by Init() exactly once. The context
// is destroyed; do not delete or reuse its pointer. A null context is a no-op.
// Restores FFmpeg's default log callback and its pre-Init log level.
MW_STREAMER_API void Shutdown(MwStreamerContext* context);

}  // namespace mw::streamer

#endif  // MW_STREAMER_INIT_INIT_H_

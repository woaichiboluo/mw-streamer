#ifndef MW_STREAMER_INPUT_AV_PROBE_H_
#define MW_STREAMER_INPUT_AV_PROBE_H_

#include <exception>
#include <optional>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/input/av_input.h"

namespace mw::streamer {

struct AvProbeResult {
  // Present only on success. Owns deep copies independent of the connection.
  std::optional<AvInputInfo> info;
  // Present only on failure; retains the original exception and FFmpeg code.
  std::exception_ptr error;
};

class MW_STREAMER_API AvProbe final {
 public:
  // Synchronously opens and probes once, then closes the connection. Does not
  // retry, decode for playback, or start workers. Independent calls may run
  // concurrently. Opening and stream analysis share a 30-second deadline,
  // enforced cooperatively through FFmpeg's interrupt callback; underlying
  // operations that do not check it may take longer. Timeout is reported as
  // FfmpegException with AVERROR(ETIMEDOUT). All failures, including invalid
  // URLs, are returned through error rather than thrown. A later playback
  // connection may observe different source properties.
  [[nodiscard]] static AvProbeResult Probe(std::string_view url) noexcept;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_PROBE_H_

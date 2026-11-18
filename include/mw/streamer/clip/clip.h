#ifndef MW_STREAMER_CLIP_CLIP_H_
#define MW_STREAMER_CLIP_CLIP_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace mw::streamer {

struct ClipConfig {
  // Live-only, fixed for the pipeline run. Disabled allocates no clip module.
  bool enabled = false;
  // GOP-aligned history can exceed this duration by one GOP, subject to bytes.
  std::chrono::milliseconds cache_duration{30000};
  std::size_t max_cache_bytes = 64 * 1024 * 1024;
};

struct ClipTime {
  // Capture the generation when the event's source frame enters processing,
  // not when delayed detection publishes its result.
  std::uint64_t generation = 0;
  std::int64_t pts = AV_NOPTS_VALUE;
  AVRational time_base{0, 1};
};

struct ClipRequest {
  std::string event_id;
  ClipTime time;
  std::chrono::milliseconds before{0};
  std::chrono::milliseconds after{0};
  // Local MP4 path; the parent directory must exist. Each event owns its path.
  std::string path;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_CLIP_CLIP_H_

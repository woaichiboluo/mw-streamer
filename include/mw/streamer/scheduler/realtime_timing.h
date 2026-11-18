#ifndef MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_
#define MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_

#include <chrono>
#include <optional>

namespace mw::streamer {

struct RealtimeConfig {
  // Reanchor a late track only when its source PTS also jumps backward beyond
  // this tolerance. Processing overload keeps the shared realtime reference
  // so audio and video can catch up once throughput recovers.
  std::chrono::microseconds negative_time_tolerance{250000};
  std::optional<std::chrono::microseconds> negative_time_discard;
  std::chrono::microseconds discontinuity_threshold{1000000};
  // The anchoring frame passes immediately; this
  // margin buffers subsequent frames by shifting the shared clock mapping.
  std::chrono::microseconds jitter_margin{0};
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_

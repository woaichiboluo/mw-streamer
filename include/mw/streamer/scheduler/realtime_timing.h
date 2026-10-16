#ifndef MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_
#define MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_

#include <chrono>
#include <cstdint>
#include <optional>

#include "mw/export.h"

namespace mw::streamer {

struct RealtimeConfig {
  std::chrono::microseconds negative_time_tolerance{250000};
  std::optional<std::chrono::microseconds> negative_time_discard;
  std::chrono::microseconds discontinuity_threshold{1000000};
  // The anchoring frame passes immediately; this
  // margin buffers subsequent frames by shifting the shared clock mapping.
  std::chrono::microseconds jitter_margin{0};
};

namespace internal {

// Passive realtime/team clock. One Scheduler thread calls this shared instance
// for both tracks, preserving their relative PTS. Clock arguments are elapsed
// steady-clock microseconds. Invalid config throws invalid_argument; arithmetic
// overflow throws overflow_error without changing the mapping.
class MW_STREAMER_API RealtimeTiming final {
 public:
  explicit RealtimeTiming(RealtimeConfig config);

  struct Decision {
    bool ready = false;
    bool discard = false;
    std::int64_t wake_us = 0;
  };

  // A future deadline leaves input unconsumed and does not change the mapping.
  // Only ready/discard allows the owner to advance that track's input.
  Decision Schedule(std::int64_t pts_us, std::int64_t now_us);
  void Reset();

 private:
  RealtimeConfig config_;
  std::optional<std::int64_t> offset_us_;
};

}  // namespace internal
}  // namespace mw::streamer

#endif  // MW_STREAMER_SCHEDULER_REALTIME_TIMING_H_

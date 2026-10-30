#ifndef MW_SRC_STREAMER_SCHEDULER_REALTIME_TIMING_H_
#define MW_SRC_STREAMER_SCHEDULER_REALTIME_TIMING_H_

#include <cstdint>
#include <optional>

#include "mw/export.h"
#include "mw/streamer/scheduler/realtime_timing.h"

namespace mw::streamer::internal {

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

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_SCHEDULER_REALTIME_TIMING_H_

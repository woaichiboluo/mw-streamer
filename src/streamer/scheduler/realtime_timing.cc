#include "streamer/scheduler/realtime_timing.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

std::int64_t Subtract(std::int64_t a, std::int64_t b) {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  if ((b > 0 && a < kMin + b) || (b < 0 && a > kMax + b)) {
    throw std::overflow_error("Scheduler 实时时钟计算溢出");
  }
  return a - b;
}

}  // namespace

RealtimeTiming::RealtimeTiming(RealtimeConfig config) : config_(config) {
  if (config_.negative_time_tolerance.count() < 0 ||
      config_.discontinuity_threshold.count() <= 0 ||
      config_.jitter_margin.count() < 0 ||
      config_.jitter_margin >= config_.discontinuity_threshold ||
      (config_.negative_time_discard &&
       (config_.negative_time_discard->count() < 0 ||
        *config_.negative_time_discard >= config_.negative_time_tolerance))) {
    throw std::invalid_argument("Scheduler 实时调度时间阈值无效");
  }
}

RealtimeTiming::Decision RealtimeTiming::Schedule(std::int64_t pts_us,
                                                  std::int64_t now_us) {
  bool reset = !offset_us_;
  bool forward_jump = false;
  if (offset_us_) {
    const auto due_us = Subtract(pts_us, *offset_us_);
    const auto difference_us = Subtract(due_us, now_us);
    if (difference_us < -config_.negative_time_tolerance.count()) {
      reset = true;
    } else if (difference_us >= config_.discontinuity_threshold.count()) {
      reset = true;
      forward_jump = true;
    } else if (config_.negative_time_discard &&
               difference_us < -config_.negative_time_discard->count()) {
      MW_LOG_DEBUG("streamer",
                   "Scheduler 丢弃迟到输入: pts_us={} now_us={} diff_us={}",
                   pts_us, now_us, difference_us);
      return {false, true, now_us};
    } else if (difference_us > 0) {
      // Both deadlines and clock reads use whole microseconds. A returned
      // deadline is strictly future, including for fractional video rates.
      return {false, false, due_us};
    }
  }
  if (reset) {
    const auto candidate =
        Subtract(Subtract(pts_us, now_us), config_.jitter_margin.count());
    // RealtimeTeam chooses the smaller offset so late audio/video cannot
    // accelerate the other track. A forward discontinuity resets the team.
    const auto offset = !offset_us_ || forward_jump
                            ? candidate
                            : std::min(*offset_us_, candidate);
    MW_LOG_INFO("streamer",
                "Scheduler 建立或重同步实时映射: pts_us={} now_us={} "
                "forward_jump={} offset_us={}",
                pts_us, now_us, forward_jump, offset);
    offset_us_ = offset;
  }
  return {true, false, now_us};
}

void RealtimeTiming::Reset() { offset_us_.reset(); }

}  // namespace mw::streamer::internal

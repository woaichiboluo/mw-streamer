#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_LATENCY_SNAPSHOT_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_LATENCY_SNAPSHOT_H_

#include <chrono>
#include <cstdint>

namespace mw::streamer {

struct LatencySnapshot {
  std::uint64_t sample_count = 0;
  std::chrono::microseconds p50{0};
  std::chrono::microseconds p95{0};
  std::chrono::microseconds p99{0};
  std::chrono::microseconds max{0};
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_LATENCY_SNAPSHOT_H_

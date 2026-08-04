#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_REMUX_COLLECTOR_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_REMUX_COLLECTOR_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "mw/performance/snapshot.h"

namespace mw::streamer::performance::internal {

class RemuxCollector final {
 public:
  RemuxCollector() = default;

  RemuxCollector(const RemuxCollector&) = delete;
  RemuxCollector& operator=(const RemuxCollector&) = delete;

  void Reset();
  void RecordPacket(std::size_t bytes);
  RemuxPipelineSnapshot Collect(std::size_t output_queue_depth,
                                NetworkInputSnapshot input,
                                std::vector<NetworkOutputSnapshot> outputs);

 private:
  std::mutex collection_mutex_;
  std::chrono::steady_clock::time_point last_collection_ =
      std::chrono::steady_clock::now();
  std::uint64_t packets_ = 0;
  std::uint64_t bytes_ = 0;
};

}  // namespace mw::streamer::performance::internal

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_REMUX_COLLECTOR_H_

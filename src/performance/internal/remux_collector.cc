#include "mw/performance/internal/remux_collector.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace mw::streamer::performance::internal {

void RemuxCollector::Reset() {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  packets_ = 0;
  bytes_ = 0;
  last_collection_ = std::chrono::steady_clock::now();
}

void RemuxCollector::RecordPacket(std::size_t bytes) {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  ++packets_;
  bytes_ += static_cast<std::uint64_t>(bytes);
}

RemuxPipelineSnapshot RemuxCollector::Collect(
    std::size_t output_queue_depth, NetworkInputSnapshot input,
    std::vector<NetworkOutputSnapshot> outputs) {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto interval = now - last_collection_;
  last_collection_ = now;
  const auto packets = std::exchange(packets_, 0);
  const auto bytes = std::exchange(bytes_, 0);
  const double seconds = std::chrono::duration<double>(interval).count();

  RemuxPipelineSnapshot snapshot;
  snapshot.interval =
      std::chrono::duration_cast<std::chrono::nanoseconds>(interval);
  snapshot.input = input;
  snapshot.outputs = std::move(outputs);
  snapshot.packets = packets;
  snapshot.bytes = bytes;
  snapshot.bits_per_second =
      seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds : 0.0;
  snapshot.output_queue_depth = output_queue_depth;
  return snapshot;
}

}  // namespace mw::streamer::performance::internal

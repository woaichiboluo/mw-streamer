#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_PIPELINE_SNAPSHOT_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_PIPELINE_SNAPSHOT_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "mw/performance/latency_snapshot.h"

namespace mw::streamer::performance {

enum class PerformanceType {
  kInput,
  kAudioDecoder,
  kVideoDecoder,
  kAudioProcessor,
  kVideoProcessor,
  kSynchronizer,
  kAudioEncoder,
  kVideoEncoder,
  kRemux,
};

enum class PerformanceUnit { kNone, kPacket, kFrame, kSample };

// Single-target network output state reported by the publishing backend.
// File recording leaves the network fields at their defaults.
struct NetworkOutputSnapshot {
  std::string target;
  bool connected = false;
  std::uint64_t reconnect_count = 0;
  std::uint64_t sent_bytes = 0;
};

// Counters accumulate for the owning object's lifetime, including reconnects.
// Input means media entering actual processing, not merely entering a queue.
// Output counts production once, independently of the number of consumers.
// A zero-valued item describes supported instrumentation, not track presence.
struct OperationSnapshot {
  PerformanceType type = PerformanceType::kInput;
  PerformanceUnit input_unit = PerformanceUnit::kNone;
  PerformanceUnit output_unit = PerformanceUnit::kNone;
  std::uint64_t input_count = 0;
  std::uint64_t output_count = 0;
  std::uint64_t input_bytes = 0;
  std::uint64_t output_bytes = 0;
  std::uint64_t started_calls = 0;
  // Includes failures; started_calls == completed_calls + in_flight.
  std::uint64_t completed_calls = 0;
  std::uint64_t failed_calls = 0;
  std::uint64_t in_flight = 0;
  // Host-side processing time, excluding explicitly suspended downstream calls.
  // No GPU synchronization is inserted. Active calls contribute after
  // completion.
  std::chrono::nanoseconds total_time{0};
  std::chrono::nanoseconds max_time{0};
  // Cumulative distribution, NOT a difference between sampling windows.
  LatencySnapshot lifetime_latency;

  // Filled only by WithRatesSince; raw counters above remain cumulative.
  bool rates_available = false;
  double input_per_second = 0.0;
  double output_per_second = 0.0;
  double input_bytes_per_second = 0.0;
  double output_bytes_per_second = 0.0;
  double calls_per_second = 0.0;
  std::chrono::nanoseconds interval_mean_time{0};
};

struct NodeSnapshot {
  // Pipeline assigns stable paths, e.g. sink/0/0. Direct sink queries leave
  // this empty. Names are descriptive; callers must use type and id for
  // selection.
  std::string id;
  std::string name;
  std::vector<OperationSnapshot> operations;
  std::vector<NodeSnapshot> downstream;
};

struct PerformanceMatch {
  const NodeSnapshot* node;
  const OperationSnapshot* operation;
};

// An owned value independent of Pipeline/Sink lifetime. Nodes are sampled in
// traversal order, not as a globally atomic media boundary. Querying does not
// change counters, sampling windows or the live pipeline.
struct PipelineSnapshot {
  std::uint64_t pipeline_id = 0;
  std::chrono::steady_clock::time_point sampled_at{};
  std::chrono::nanoseconds interval{0};
  NodeSnapshot input;
  std::vector<NodeSnapshot> sinks;

  // Preorder matches borrow this snapshot. Do not mutate/move/destroy it while
  // using the results; querying a temporary is deliberately disallowed.
  std::vector<PerformanceMatch> Find(PerformanceType type) const&;
  std::vector<PerformanceMatch> Find(PerformanceType type) const&& = delete;

  // Pure window calculation, so independent readers keep independent baselines.
  // Requires a strictly earlier snapshot from the same pipeline. Throws
  // invalid_argument for incompatible topology or decreasing counters.
  // Each reader must acquire its own snapshots sequentially; overlapping tree
  // traversals do not establish a consistent before/after order for all nodes.
  // Lifetime histogram percentiles cannot be subtracted and stay cumulative.
  PipelineSnapshot WithRatesSince(const PipelineSnapshot& previous) const;
};

}  // namespace mw::streamer::performance

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_PIPELINE_SNAPSHOT_H_

#include "mw/streamer/performance/pipeline_snapshot.h"

#include <stdexcept>

namespace mw::streamer {
namespace {

void FindInNode(const NodeSnapshot& node, PerformanceType type,
                std::vector<PerformanceMatch>& matches) {
  for (const auto& operation : node.operations) {
    if (operation.type == type) matches.push_back({&node, &operation});
  }
  for (const auto& child : node.downstream) FindInNode(child, type, matches);
}

std::uint64_t Delta(std::uint64_t current, std::uint64_t previous) {
  if (current < previous) {
    throw std::invalid_argument("性能快照累计计数不能回退");
  }
  return current - previous;
}

void CalculateRates(OperationSnapshot& current,
                    const OperationSnapshot& previous, double seconds) {
  if (current.type != previous.type ||
      current.input_unit != previous.input_unit ||
      current.output_unit != previous.output_unit ||
      current.total_time < previous.total_time) {
    throw std::invalid_argument("性能快照处理项不兼容");
  }
  current.input_per_second =
      Delta(current.input_count, previous.input_count) / seconds;
  current.output_per_second =
      Delta(current.output_count, previous.output_count) / seconds;
  current.input_bytes_per_second =
      Delta(current.input_bytes, previous.input_bytes) / seconds;
  current.output_bytes_per_second =
      Delta(current.output_bytes, previous.output_bytes) / seconds;
  const auto calls = Delta(current.completed_calls, previous.completed_calls);
  static_cast<void>(Delta(current.started_calls, previous.started_calls));
  static_cast<void>(Delta(current.failed_calls, previous.failed_calls));
  current.calls_per_second = calls / seconds;
  const auto elapsed = current.total_time - previous.total_time;
  const auto mean =
      calls == 0 ? 0 : static_cast<std::uint64_t>(elapsed.count()) / calls;
  current.interval_mean_time = std::chrono::nanoseconds(mean);
  current.rates_available = true;
}

void CalculateNodeRates(NodeSnapshot& current, const NodeSnapshot& previous,
                        double seconds) {
  if (current.id != previous.id || current.name != previous.name ||
      current.operations.size() != previous.operations.size() ||
      current.downstream.size() != previous.downstream.size()) {
    throw std::invalid_argument("性能快照链路结构不兼容");
  }
  for (std::size_t i = 0; i < current.operations.size(); ++i) {
    CalculateRates(current.operations[i], previous.operations[i], seconds);
  }
  for (std::size_t i = 0; i < current.downstream.size(); ++i) {
    CalculateNodeRates(current.downstream[i], previous.downstream[i], seconds);
  }
}

}  // namespace

std::vector<PerformanceMatch> PipelineSnapshot::Find(
    PerformanceType type) const& {
  std::vector<PerformanceMatch> result;
  FindInNode(input, type, result);
  for (const auto& sink : sinks) FindInNode(sink, type, result);
  return result;
}

PipelineSnapshot PipelineSnapshot::WithRatesSince(
    const PipelineSnapshot& previous) const {
  if (pipeline_id == 0 || pipeline_id != previous.pipeline_id ||
      sampled_at <= previous.sampled_at ||
      sinks.size() != previous.sinks.size()) {
    throw std::invalid_argument("性能速率需要同一Pipeline的前后两份快照");
  }
  auto result = *this;
  result.interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
      sampled_at - previous.sampled_at);
  const auto seconds = std::chrono::duration<double>(result.interval).count();
  CalculateNodeRates(result.input, previous.input, seconds);
  for (std::size_t i = 0; i < result.sinks.size(); ++i) {
    CalculateNodeRates(result.sinks[i], previous.sinks[i], seconds);
  }
  return result;
}

}  // namespace mw::streamer

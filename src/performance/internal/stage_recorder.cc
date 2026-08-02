#include "mw/performance/internal/stage_recorder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace mw::streamer::performance::internal {
namespace {

constexpr std::int64_t kMinimumLatencyMicroseconds = 1;
constexpr std::int64_t kMaximumLatencyMicroseconds =
    60LL * 60LL * 1000LL * 1000LL;
constexpr int kSignificantFigures = 3;

std::chrono::microseconds Percentile(const hdr_histogram& histogram,
                                     double percentile) noexcept {
  return std::chrono::microseconds(
      hdr_value_at_percentile(&histogram, percentile));
}

}  // namespace

StageRecorder::StageRecorder() {
  const int result = hdr_interval_recorder_init_all(
      &latency_, kMinimumLatencyMicroseconds, kMaximumLatencyMicroseconds,
      kSignificantFigures);
  if (result != 0) {
    hdr_interval_recorder_destroy(&latency_);
    throw std::runtime_error("初始化HdrHistogram失败");
  }

  hdr_interval_recorder_sample(&latency_);
  if (!latency_.active || !latency_.inactive) {
    hdr_interval_recorder_destroy(&latency_);
    throw std::runtime_error("预分配HdrHistogram失败");
  }
}

StageRecorder::~StageRecorder() { hdr_interval_recorder_destroy(&latency_); }

void StageRecorder::Record(std::uint64_t units,
                           std::chrono::nanoseconds latency) noexcept {
  completed_units_.fetch_add(units, std::memory_order_relaxed);
  const auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(latency).count();
  const auto bounded = std::clamp(microseconds, kMinimumLatencyMicroseconds,
                                  kMaximumLatencyMicroseconds);
  static_cast<void>(hdr_interval_recorder_record_value(&latency_, bounded));
}

StageInterval StageRecorder::Collect() noexcept {
  StageInterval interval;
  interval.units = completed_units_.exchange(0, std::memory_order_relaxed);

  const auto* histogram = hdr_interval_recorder_sample(&latency_);
  if (!histogram) {
    return interval;
  }
  interval.latency.sample_count =
      static_cast<std::uint64_t>(histogram->total_count);
  if (histogram->total_count == 0) {
    return interval;
  }
  interval.latency.p50 = Percentile(*histogram, 50.0);
  interval.latency.p95 = Percentile(*histogram, 95.0);
  interval.latency.p99 = Percentile(*histogram, 99.0);
  interval.latency.max = std::chrono::microseconds(hdr_max(histogram));
  return interval;
}

}  // namespace mw::streamer::performance::internal

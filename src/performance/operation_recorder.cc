#include "mw/performance/operation_recorder.h"

#include <algorithm>
#include <exception>
#include <stdexcept>

extern "C" {
#include <hdr/hdr_histogram.h>
}

namespace mw::streamer {
namespace {

constexpr std::int64_t kMaximumLatencyMicroseconds = 60LL * 60 * 1000 * 1000;

}  // namespace

OperationRecorder::OperationRecorder(PerformanceType type,
                                     PerformanceUnit input_unit,
                                     PerformanceUnit output_unit) {
  snapshot_.type = type;
  snapshot_.input_unit = input_unit;
  snapshot_.output_unit = output_unit;
  // Record microsecond latencies up to one hour with three significant digits.
  // One histogram suffices because reads are nondestructive and serialized.
  if (hdr_init(1, kMaximumLatencyMicroseconds, 3, &histogram_) != 0) {
    throw std::runtime_error("初始化Sink调用统计HdrHistogram失败");
  }
}

OperationRecorder::~OperationRecorder() { hdr_close(histogram_); }

void OperationRecorder::AddInput(std::uint64_t count,
                                 std::uint64_t bytes) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.input_count += count;
  snapshot_.input_bytes += bytes;
}

void OperationRecorder::AddOutput(std::uint64_t count,
                                  std::uint64_t bytes) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.output_count += count;
  snapshot_.output_bytes += bytes;
}

OperationSnapshot OperationRecorder::GetSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = snapshot_;
  auto& latency = result.lifetime_latency;
  latency.sample_count = snapshot_.completed_calls;
  if (latency.sample_count != 0) {
    latency.p50 =
        std::chrono::microseconds(hdr_value_at_percentile(histogram_, 50));
    latency.p95 =
        std::chrono::microseconds(hdr_value_at_percentile(histogram_, 95));
    latency.p99 =
        std::chrono::microseconds(hdr_value_at_percentile(histogram_, 99));
    latency.max = std::chrono::microseconds(hdr_max(histogram_));
  }
  return result;
}

void OperationRecorder::BeginCall() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  ++snapshot_.started_calls;
  ++snapshot_.in_flight;
}

void OperationRecorder::EndCall(std::chrono::nanoseconds elapsed,
                                bool failed) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  --snapshot_.in_flight;
  ++snapshot_.completed_calls;
  snapshot_.failed_calls += failed ? 1 : 0;
  snapshot_.total_time += elapsed;
  snapshot_.max_time = std::max(snapshot_.max_time, elapsed);
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
  hdr_record_value(
      histogram_,
      std::clamp<std::int64_t>(micros.count(), 1, kMaximumLatencyMicroseconds));
}

OperationRecorder::Call::Call(OperationRecorder& recorder) noexcept
    : recorder_(recorder), exceptions_(std::uncaught_exceptions()) {
  recorder_.BeginCall();
  started_at_ = std::chrono::steady_clock::now();
}

OperationRecorder::Call::Call(OperationRecorder& recorder,
                              Call*& active_slot) noexcept
    : Call(recorder) {
  active_slot_ = &active_slot;
  previous_call_ = active_slot;
  active_slot = this;
}

OperationRecorder::Call::~Call() { Finish(); }

void OperationRecorder::Call::Pause() noexcept {
  if (finished_) return;
  if (pause_depth_++ == 0) {
    elapsed_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started_at_);
  }
}

void OperationRecorder::Call::Resume() noexcept {
  if (finished_ || pause_depth_ == 0) return;
  if (--pause_depth_ == 0) started_at_ = std::chrono::steady_clock::now();
}

void OperationRecorder::Call::Finish() noexcept {
  if (finished_) return;
  Pause();
  finished_ = true;
  if (active_slot_) *active_slot_ = previous_call_;
  recorder_.EndCall(elapsed_, std::uncaught_exceptions() > exceptions_);
}

}  // namespace mw::streamer

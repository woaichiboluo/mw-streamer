#include "mw/performance/operation_recorder.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using mw::streamer::performance::OperationRecorder;
using mw::streamer::performance::PerformanceType;
using mw::streamer::performance::PerformanceUnit;

}  // namespace

TEST_CASE("OperationRecorder累计输入输出并区分进行中与失败调用") {
  OperationRecorder recorder(PerformanceType::kVideoEncoder,
                             PerformanceUnit::kFrame, PerformanceUnit::kPacket);
  recorder.AddInput(2, 100);
  recorder.AddOutput(3, 80);
  {
    OperationRecorder::Call call(recorder);
    const auto active = recorder.GetSnapshot();
    CHECK(active.type == PerformanceType::kVideoEncoder);
    CHECK(active.input_unit == PerformanceUnit::kFrame);
    CHECK(active.output_unit == PerformanceUnit::kPacket);
    CHECK(active.input_count == 2);
    CHECK(active.input_bytes == 100);
    CHECK(active.output_count == 3);
    CHECK(active.output_bytes == 80);
    CHECK(active.started_calls == 1);
    CHECK(active.completed_calls == 0);
    CHECK(active.in_flight == 1);
    CHECK(active.total_time == std::chrono::nanoseconds::zero());
  }
  CHECK_THROWS_AS(
      [&]() {
        OperationRecorder::Call call(recorder);
        throw std::runtime_error("processing failed");
      }(),
      std::runtime_error);
  const auto completed = recorder.GetSnapshot();
  CHECK(completed.started_calls == 2);
  CHECK(completed.completed_calls == 2);
  CHECK(completed.failed_calls == 1);
  CHECK(completed.in_flight == 0);
  CHECK(completed.lifetime_latency.sample_count == 2);
  CHECK(completed.total_time >= completed.max_time);
}

TEST_CASE("OperationRecorder提前结束幂等且不归因后续异常") {
  OperationRecorder recorder(PerformanceType::kVideoProcessor,
                             PerformanceUnit::kFrame, PerformanceUnit::kFrame);
  {
    OperationRecorder::Call call(recorder);
    call.Finish();
    const auto once = recorder.GetSnapshot();
    call.Finish();
    call.Pause();
    call.Resume();
    const auto twice = recorder.GetSnapshot();
    CHECK(twice.completed_calls == 1);
    CHECK(twice.total_time == once.total_time);
    CHECK(twice.max_time == once.max_time);
  }
  CHECK_THROWS_AS(
      [&]() {
        OperationRecorder::Call call(recorder);
        call.Finish();
        throw std::runtime_error("downstream failed");
      }(),
      std::runtime_error);
  const auto completed = recorder.GetSnapshot();
  CHECK(completed.completed_calls == 2);
  CHECK(completed.failed_calls == 0);
  CHECK(completed.in_flight == 0);
}

TEST_CASE("OperationRecorder嵌套调用恢复活动槽并支持嵌套暂停") {
  OperationRecorder recorder(PerformanceType::kAudioDecoder,
                             PerformanceUnit::kPacket,
                             PerformanceUnit::kSample);
  OperationRecorder::Call* active = nullptr;
  {
    OperationRecorder::Call outer(recorder, active);
    CHECK(active == &outer);
    {
      OperationRecorder::Suspension pause_outer(active);
      OperationRecorder::Call inner(recorder, active);
      CHECK(active == &inner);
      CHECK(recorder.GetSnapshot().in_flight == 2);
      {
        OperationRecorder::Suspension first(active);
        OperationRecorder::Suspension second(active);
        inner.Pause();
        inner.Resume();
        inner.Finish();
        CHECK(active == &outer);
        const auto paused = recorder.GetSnapshot();
        inner.Resume();
        inner.Pause();
        inner.Finish();
        CHECK(recorder.GetSnapshot().total_time == paused.total_time);
        CHECK(recorder.GetSnapshot().completed_calls == 1);
      }
      CHECK(active == &outer);
    }
    CHECK(recorder.GetSnapshot().in_flight == 1);
    outer.Finish();
    CHECK(active == nullptr);
  }
  OperationRecorder::Suspension unbound(nullptr);
  CHECK(active == nullptr);
  CHECK(recorder.GetSnapshot().completed_calls == 2);
  CHECK(recorder.GetSnapshot().failed_calls == 0);
  CHECK(recorder.GetSnapshot().in_flight == 0);
}

TEST_CASE("OperationRecorder异常展开恢复活动槽且暂停异常仍计失败") {
  OperationRecorder recorder(PerformanceType::kAudioProcessor,
                             PerformanceUnit::kSample,
                             PerformanceUnit::kSample);
  OperationRecorder::Call* active = nullptr;
  {
    OperationRecorder::Call outer(recorder, active);
    CHECK_THROWS_AS(
        [&]() {
          OperationRecorder::Call inner(recorder, active);
          inner.Pause();
          throw std::runtime_error("inner failed");
        }(),
        std::runtime_error);
    CHECK(active == &outer);
    const auto failed = recorder.GetSnapshot();
    CHECK(failed.started_calls == 2);
    CHECK(failed.completed_calls == 1);
    CHECK(failed.failed_calls == 1);
    CHECK(failed.in_flight == 1);
  }
  CHECK(active == nullptr);
  const auto completed = recorder.GetSnapshot();
  CHECK(completed.completed_calls == 2);
  CHECK(completed.failed_calls == 1);
  CHECK(completed.in_flight == 0);
}

TEST_CASE("OperationRecorder分位快照只读且保留累计分布") {
  OperationRecorder recorder(PerformanceType::kRemux, PerformanceUnit::kPacket,
                             PerformanceUnit::kPacket);
  const auto empty = recorder.GetSnapshot();
  CHECK(empty.lifetime_latency.sample_count == 0);
  CHECK(empty.lifetime_latency.p99 == std::chrono::microseconds::zero());
  for (int index = 0; index < 100; ++index) {
    OperationRecorder::Call call(recorder);
    call.Pause();
  }
  const auto first = recorder.GetSnapshot();
  const auto second = recorder.GetSnapshot();
  CHECK(first.lifetime_latency.sample_count == 100);
  CHECK(first.lifetime_latency.p50 <= first.lifetime_latency.p95);
  CHECK(first.lifetime_latency.p95 <= first.lifetime_latency.p99);
  CHECK(first.lifetime_latency.p99 <= first.lifetime_latency.max);
  CHECK(first.lifetime_latency.p50 >= std::chrono::microseconds(1));
  CHECK(second.completed_calls == first.completed_calls);
  CHECK(second.total_time == first.total_time);
  CHECK(second.lifetime_latency.sample_count ==
        first.lifetime_latency.sample_count);
  CHECK(second.lifetime_latency.p50 == first.lifetime_latency.p50);
  CHECK(second.lifetime_latency.p95 == first.lifetime_latency.p95);
  CHECK(second.lifetime_latency.p99 == first.lifetime_latency.p99);
  CHECK(second.lifetime_latency.max == first.lifetime_latency.max);
  { OperationRecorder::Call call(recorder); }
  const auto next = recorder.GetSnapshot();
  CHECK(next.completed_calls == 101);
  CHECK(next.lifetime_latency.sample_count == 101);
}

TEST_CASE("OperationRecorder多线程记录与并发采集不丢失累计量") {
  constexpr int kWriters = 4;
  constexpr int kCallsPerWriter = 128;
  constexpr int kFailureInterval = 16;
  OperationRecorder recorder(PerformanceType::kVideoDecoder,
                             PerformanceUnit::kPacket, PerformanceUnit::kFrame);
  std::promise<void> start;
  const auto gate = start.get_future().share();
  std::atomic<bool> consistent{true};
  std::vector<std::thread> threads;
  for (int writer = 0; writer < kWriters; ++writer) {
    threads.emplace_back([&]() {
      gate.wait();
      for (int index = 0; index < kCallsPerWriter; ++index) {
        recorder.AddInput(2, 120);
        try {
          OperationRecorder::Call call(recorder);
          if (index % kFailureInterval == 0) {
            throw std::runtime_error("expected failure");
          }
          recorder.AddOutput(1, 80);
        } catch (const std::runtime_error&) {
        }
      }
    });
  }
  for (int reader = 0; reader < 2; ++reader) {
    threads.emplace_back([&]() {
      gate.wait();
      std::uint64_t previous_completed = 0;
      std::uint64_t previous_input = 0;
      for (int index = 0; index < kCallsPerWriter; ++index) {
        const auto snapshot = recorder.GetSnapshot();
        if (snapshot.started_calls !=
                snapshot.completed_calls + snapshot.in_flight ||
            snapshot.completed_calls < previous_completed ||
            snapshot.input_count < previous_input ||
            snapshot.failed_calls > snapshot.completed_calls ||
            snapshot.lifetime_latency.sample_count !=
                snapshot.completed_calls) {
          consistent.store(false);
        }
        previous_completed = snapshot.completed_calls;
        previous_input = snapshot.input_count;
      }
    });
  }
  start.set_value();
  for (auto& thread : threads) {
    thread.join();
  }
  const auto snapshot = recorder.GetSnapshot();
  constexpr auto kCalls = kWriters * kCallsPerWriter;
  constexpr auto kFailures = kCalls / kFailureInterval;
  CHECK(consistent.load());
  CHECK(snapshot.input_count == kCalls * 2);
  CHECK(snapshot.input_bytes == kCalls * 120);
  CHECK(snapshot.output_count == kCalls - kFailures);
  CHECK(snapshot.output_bytes == (kCalls - kFailures) * 80);
  CHECK(snapshot.started_calls == kCalls);
  CHECK(snapshot.completed_calls == kCalls);
  CHECK(snapshot.failed_calls == kFailures);
  CHECK(snapshot.in_flight == 0);
  CHECK(snapshot.lifetime_latency.sample_count == kCalls);
}

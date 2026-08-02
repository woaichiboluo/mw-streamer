#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "mw/performance/internal/stage_recorder.h"
#include "mw/performance/internal/streaming_collector.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::performance::NetworkInputSnapshot;
using mw::streamer::performance::NetworkOutputSnapshot;
using mw::streamer::performance::internal::StageRecorder;
using mw::streamer::performance::internal::StreamingCollector;

}  // namespace

TEST_CASE("StageRecorder采集当前窗口并在采集后清空") {
  StageRecorder recorder;
  recorder.Record(3, 1ms);
  recorder.Record(2, 2ms);

  const auto first = recorder.Collect();
  CHECK(first.units == 5);
  CHECK(first.latency.sample_count == 2);
  CHECK(first.latency.p50 >= 1ms);
  CHECK(first.latency.p95 >= 2ms);
  CHECK(first.latency.p99 >= 2ms);
  CHECK(first.latency.max >= 2ms);

  const auto second = recorder.Collect();
  CHECK(second.units == 0);
  CHECK(second.latency.sample_count == 0);
  CHECK(second.latency.p50 == 0us);
  CHECK(second.latency.p95 == 0us);
  CHECK(second.latency.p99 == 0us);
  CHECK(second.latency.max == 0us);
}

TEST_CASE("StageRecorder允许单写线程与采集线程并发") {
  constexpr std::uint64_t kSamples = 10'000;
  StageRecorder recorder;
  std::atomic_bool started = false;
  std::atomic_bool finished = false;
  std::thread writer([&]() {
    started.store(true, std::memory_order_release);
    for (std::uint64_t index = 0; index < kSamples; ++index) {
      recorder.Record(1, 10us);
    }
    finished.store(true, std::memory_order_release);
  });

  while (!started.load(std::memory_order_acquire)) {
  }
  std::uint64_t units = 0;
  std::uint64_t latency_samples = 0;
  while (!finished.load(std::memory_order_acquire)) {
    const auto interval = recorder.Collect();
    units += interval.units;
    latency_samples += interval.latency.sample_count;
  }
  writer.join();
  const auto final_interval = recorder.Collect();
  units += final_interval.units;
  latency_samples += final_interval.latency.sample_count;

  CHECK(units == kSamples);
  CHECK(latency_samples == kSamples);
}

TEST_CASE("StreamingCollector串行化多个并发采集者") {
  constexpr std::uint64_t kSamples = 100'000;
  StreamingCollector collector;
  std::atomic_bool start = false;
  std::atomic_bool finished = false;
  std::atomic<std::uint64_t> collected_frames = 0;
  std::atomic<std::uint64_t> collected_latencies = 0;

  std::thread writer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t index = 0; index < kSamples; ++index) {
      collector.video().decode().Record(1, 10us);
    }
    finished.store(true, std::memory_order_release);
  });
  auto collect = [&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    while (!finished.load(std::memory_order_acquire)) {
      const auto snapshot = collector.Collect(false, true, 0, 0, 0, {}, {});
      collected_frames.fetch_add(snapshot.video.decode.frames,
                                 std::memory_order_relaxed);
      collected_latencies.fetch_add(snapshot.video.decode.latency.sample_count,
                                    std::memory_order_relaxed);
    }
  };
  std::thread first_collector(collect);
  std::thread second_collector(collect);

  start.store(true, std::memory_order_release);
  writer.join();
  first_collector.join();
  second_collector.join();
  const auto final_snapshot = collector.Collect(false, true, 0, 0, 0, {}, {});
  collected_frames.fetch_add(final_snapshot.video.decode.frames,
                             std::memory_order_relaxed);
  collected_latencies.fetch_add(
      final_snapshot.video.decode.latency.sample_count,
      std::memory_order_relaxed);

  CHECK(collected_frames.load(std::memory_order_relaxed) == kSamples);
  CHECK(collected_latencies.load(std::memory_order_relaxed) == kSamples);
}

TEST_CASE("StreamingCollector生成统一窗口并保留网络原始字节") {
  StreamingCollector collector;
  collector.video().decode().Record(100, 10ms);
  collector.audio().decode().Record(100, 10ms);
  collector.Reset();
  collector.video().decode().Record(2, 500us);
  collector.video().process().Record(2, 1ms);
  collector.video().encode().Record(2, 2ms);
  collector.video().RecordDroppedPackets(3);
  collector.audio().decode().Record(2048, 100us);
  collector.audio().process().Record(2048, 200us);
  collector.audio().encode().Record(2048, 300us);
  std::this_thread::sleep_for(2ms);

  NetworkInputSnapshot input;
  input.is_network = true;
  input.connected = true;
  input.generation = 7;
  input.reconnect_count = 2;
  input.received_bytes = 1234;
  std::vector<NetworkOutputSnapshot> outputs{
      {"rtmp://127.0.0.1/live/test", true, 1, 5678},
  };
  const auto first =
      collector.Collect(true, true, 4, 5, 6, input, std::move(outputs));

  CHECK(first.interval > 0ns);
  CHECK(first.input.received_bytes == 1234);
  REQUIRE(first.outputs.size() == 1);
  CHECK(first.outputs.front().sent_bytes == 5678);
  CHECK(first.video.decode.frames == 2);
  CHECK(first.video.process.frames == 2);
  CHECK(first.video.encode.frames == 2);
  CHECK(first.video.dropped_packets == 3);
  CHECK(first.video.queue_depth == 5);
  CHECK(first.audio.decode.samples == 2048);
  CHECK(first.audio.process.samples == 2048);
  CHECK(first.audio.encode.samples == 2048);
  CHECK(first.audio.queue_depth == 4);
  CHECK(first.output_queue_depth == 6);

  std::this_thread::sleep_for(2ms);
  const auto second = collector.Collect(true, true, 0, 0, 0, {}, {});
  CHECK(second.video.decode.frames == 0);
  CHECK(second.video.process.frames == 0);
  CHECK(second.video.encode.frames == 0);
  CHECK(second.video.decode.frames_per_second == 0.0);
  CHECK(second.video.decode.latency.sample_count == 0);
  CHECK(second.audio.decode.samples == 0);
  CHECK(second.audio.decode.samples_per_second == 0.0);
  CHECK(second.audio.decode.latency.sample_count == 0);
}

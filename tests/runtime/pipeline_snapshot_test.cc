#include "mw/performance/pipeline_snapshot.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using namespace mw::streamer;

PipelineSnapshot Snapshot() {
  PipelineSnapshot snapshot;
  snapshot.pipeline_id = 1;
  snapshot.sampled_at = std::chrono::steady_clock::time_point{1s};
  snapshot.input = {"input", "Input", {}, {}};
  OperationSnapshot video;
  video.type = PerformanceType::kVideoEncoder;
  video.input_unit = PerformanceUnit::kFrame;
  video.output_unit = PerformanceUnit::kPacket;
  OperationSnapshot audio;
  audio.type = PerformanceType::kAudioEncoder;
  audio.input_unit = PerformanceUnit::kSample;
  audio.output_unit = PerformanceUnit::kPacket;
  snapshot.sinks.push_back({"sink/0",
                            "DecoderSink",
                            {},
                            {{"sink/0/0", "EncoderSink", {audio, video}, {}},
                             {"sink/0/1", "EncoderSink", {video}, {}}}});
  return snapshot;
}

OperationSnapshot& Video(PipelineSnapshot& snapshot) {
  return snapshot.sinks[0].downstream[0].operations[1];
}

}  // namespace

TEST_CASE("PipelineSnapshot按固定类型递归查找全部实例而不混合音视频") {
  const auto snapshot = Snapshot();
  const auto videos = snapshot.Find(PerformanceType::kVideoEncoder);
  REQUIRE(videos.size() == 2);
  CHECK(videos[0].node->id == "sink/0/0");
  CHECK(videos[1].node->id == "sink/0/1");
  CHECK(videos[0].operation->input_unit == PerformanceUnit::kFrame);
  const auto audios = snapshot.Find(PerformanceType::kAudioEncoder);
  REQUIRE(audios.size() == 1);
  CHECK(audios[0].node == videos[0].node);
  CHECK(audios[0].operation->input_unit == PerformanceUnit::kSample);
  CHECK(snapshot.Find(PerformanceType::kVideoDecoder).empty());
  CHECK(snapshot.Find(PerformanceType::kVideoEncoder)[0].operation ==
        videos[0].operation);
}

TEST_CASE("PipelineSnapshot速度按窗口计算且保留累计值与累计分位") {
  auto previous = Snapshot();
  Video(previous).input_count = 10;
  Video(previous).output_count = 8;
  Video(previous).completed_calls = 10;
  Video(previous).started_calls = 10;
  Video(previous).total_time = 10ms;
  auto current = previous;
  current.sampled_at += 2s;
  auto& video = Video(current);
  video.input_count += 50;
  video.output_count += 48;
  video.input_bytes += 200;
  video.output_bytes += 100;
  video.completed_calls += 50;
  video.started_calls += 51;
  video.in_flight = 1;
  video.total_time += 100ms;
  video.max_time = 50ms;
  video.lifetime_latency.p95 = 3ms;
  const auto measured = current.WithRatesSince(previous);
  REQUIRE(measured.interval == 2s);
  const auto matches = measured.Find(PerformanceType::kVideoEncoder);
  REQUIRE(matches.size() == 2);
  const auto& result = *matches[0].operation;
  CHECK(result.rates_available);
  CHECK(result.input_per_second == Catch::Approx(25));
  CHECK(result.output_per_second == Catch::Approx(24));
  CHECK(result.input_bytes_per_second == Catch::Approx(100));
  CHECK(result.output_bytes_per_second == Catch::Approx(50));
  CHECK(result.calls_per_second == Catch::Approx(25));
  CHECK(result.interval_mean_time == 2ms);
  CHECK(result.input_count == 60);
  CHECK(result.output_count == 56);
  CHECK(result.in_flight == 1);
  CHECK(result.total_time == 110ms);
  CHECK(result.max_time == 50ms);
  CHECK(result.lifetime_latency.p95 == 3ms);
  CHECK(matches[1].operation->output_per_second == 0);
  CHECK(matches[1].operation->interval_mean_time == 0ns);
  CHECK_FALSE(Video(current).rates_available);
  CHECK_FALSE(Video(previous).rates_available);
}

TEST_CASE("PipelineSnapshot不同读取者使用独立基线且停流后窗口速度归零") {
  const auto first = Snapshot();
  auto second = first;
  second.sampled_at += 1s;
  Video(second).input_count = 10;
  auto third = second;
  third.sampled_at += 1s;
  Video(third).input_count += 30;
  const auto short_window = third.WithRatesSince(second);
  const auto long_window = third.WithRatesSince(first);
  CHECK(short_window.Find(PerformanceType::kVideoEncoder)[0]
            .operation->input_per_second == Catch::Approx(30));
  CHECK(long_window.Find(PerformanceType::kVideoEncoder)[0]
            .operation->input_per_second == Catch::Approx(20));
  auto idle = third;
  idle.sampled_at += 1s;
  const auto measured = idle.WithRatesSince(third);
  CHECK(measured.Find(PerformanceType::kVideoEncoder)[0]
            .operation->input_per_second == 0);
  CHECK(
      measured.Find(PerformanceType::kVideoEncoder)[0].operation->input_count ==
      40);
}

TEST_CASE("PipelineSnapshot拒绝跨Pipeline逆序或不兼容快照") {
  auto previous = Snapshot();
  auto current = previous;
  current.sampled_at += 1s;
  SECTION("不同Pipeline") { ++current.pipeline_id; }
  SECTION("未归属Pipeline") { current.pipeline_id = previous.pipeline_id = 0; }
  SECTION("相同时间") { current.sampled_at = previous.sampled_at; }
  SECTION("逆序时间") { current.sampled_at = previous.sampled_at - 1s; }
  SECTION("改变拓扑") { current.sinks[0].downstream.pop_back(); }
  SECTION("改变节点ID") { current.sinks[0].id = "other"; }
  SECTION("改变处理类型") {
    Video(current).type = PerformanceType::kVideoDecoder;
  }
  SECTION("改变单位") { Video(current).input_unit = PerformanceUnit::kSample; }
  SECTION("计数回退") { Video(previous).input_count = 1; }
  SECTION("耗时回退") { Video(previous).total_time = 1ns; }
  REQUIRE_THROWS_AS(current.WithRatesSince(previous), std::invalid_argument);
}

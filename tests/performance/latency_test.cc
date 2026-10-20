#include "mw/streamer/performance/latency.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "mw/log.h"
#include "mw/streamer/performance/config.h"
#include "mw/streamer/performance/internal/settings.h"

namespace {

using mw::streamer::PerformanceConfig;
using mw::streamer::PerformanceMode;
using mw::streamer::PerformanceStage;
using mw::streamer::internal::GetPerformanceMode;
using mw::streamer::internal::PerformanceSettings;
using mw::streamer::internal::StageLatency;
using namespace std::chrono_literals;

mw::log::LogConfig LogConfig() {
  mw::log::LogConfig config;
  config.console.enabled = false;
  config.modules = {{"perf.input", mw::log::LogLevel::kOff}};
  return config;
}

PerformanceConfig Config(PerformanceMode mode = PerformanceMode::kSummary) {
  PerformanceConfig config;
  config.stages = {{PerformanceStage::kInput, mode}};
  return config;
}

struct Buffer {
  ~Buffer() { av_buffer_unref(&value); }
  AVBufferRef* value = nullptr;
};

}  // namespace

TEST_CASE("阶段延迟只用有效样本计算均值与最大值", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config());
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  REQUIRE(latency.enabled());
  latency.Observe(0);
  latency.Observe(1000000);
  latency.Observe(std::nullopt);
  latency.Observe(-1);
  CHECK(latency.samples().valid == 2);
  CHECK(latency.samples().unavailable == 2);
  CHECK(latency.samples().total_ns == 1000000);
  CHECK(latency.samples().max_ns == 1000000);
}

TEST_CASE("同源重复输出独立完成探针且恢复原有opaque", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config());
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  Buffer first;
  first.value = av_buffer_alloc(8);
  REQUIRE(first.value);
  auto* original = first.value->data;
  original[0] = 42;
  const auto start = StageLatency::TimePoint{} + 1s;
  latency.Attach(first.value, start);
  Buffer repeated;
  repeated.value = av_buffer_ref(first.value);
  REQUIRE(repeated.value);

  latency.Complete(first.value, start + 3ms);
  REQUIRE(first.value);
  CHECK(first.value->data == original);
  CHECK(first.value->data[0] == 42);
  latency.Complete(repeated.value, start + 7ms);
  REQUIRE(repeated.value);
  CHECK(repeated.value->data == original);
  CHECK(av_buffer_get_ref_count(first.value) == 2);
  CHECK(latency.samples().valid == 2);
  CHECK(latency.samples().unavailable == 0);
  CHECK(latency.samples().total_ns == 10000000);
  CHECK(latency.samples().max_ns == 7000000);
}

TEST_CASE("嵌套阶段各自完成且不覆盖上游探针", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config());
  StageLatency inner;
  StageLatency outer;
  inner.Start(PerformanceStage::kInput, &inner);
  outer.Start(PerformanceStage::kInput, &outer);
  Buffer buffer;
  const auto start = StageLatency::TimePoint{} + 1s;
  inner.Attach(buffer.value, start);
  outer.Attach(buffer.value, start + 1ms);
  outer.Complete(buffer.value, start + 4ms);
  REQUIRE(buffer.value);
  CHECK(outer.samples().total_ns == 3000000);
  inner.Complete(buffer.value, start + 5ms);
  CHECK(buffer.value == nullptr);
  CHECK(inner.samples().total_ns == 5000000);
}

TEST_CASE("外部或其它会话opaque不能冒充本阶段关联", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config());
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  Buffer foreign;
  foreign.value = av_buffer_alloc(8);
  REQUIRE(foreign.value);
  auto* original = foreign.value;
  latency.Complete(foreign.value);
  CHECK(foreign.value == original);
  CHECK(latency.samples().unavailable == 1);

  Buffer previous_session;
  latency.Attach(previous_session.value, latency.Begin());
  auto* old_probe = previous_session.value;
  latency.Start(PerformanceStage::kInput, &latency);
  latency.Complete(previous_session.value);
  CHECK(previous_session.value == old_probe);
  CHECK(latency.samples().valid == 0);
  CHECK(latency.samples().unavailable == 1);
}

TEST_CASE("结束时间倒退不能成为零延迟成功样本", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config());
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  Buffer buffer;
  const auto start = StageLatency::TimePoint{} + 1s;
  latency.Attach(buffer.value, start);
  latency.Complete(buffer.value, start - 1ms);
  CHECK(buffer.value == nullptr);
  CHECK(latency.samples().valid == 0);
  CHECK(latency.samples().unavailable == 1);
}

TEST_CASE("关闭统计不会创建探针或修改opaque", "[latency][unit]") {
  mw::log::Logging logging(LogConfig());
  PerformanceSettings settings(Config(PerformanceMode::kOff));
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  CHECK_FALSE(latency.enabled());
  CHECK(latency.Begin() == StageLatency::TimePoint{});
  Buffer buffer;
  buffer.value = av_buffer_alloc(8);
  REQUIRE(buffer.value);
  auto* original = buffer.value;
  latency.Attach(buffer.value, {});
  latency.Complete(buffer.value);
  latency.End({});
  latency.Observe(1000000);
  latency.Finish();
  latency.Finish();
  CHECK(buffer.value == original);
  CHECK(latency.samples().valid == 0);
  CHECK(latency.samples().unavailable == 0);
}

TEST_CASE("性能配置默认关闭且只开启选中阶段", "[latency][unit]") {
  CHECK(GetPerformanceMode(PerformanceStage::kInput) == PerformanceMode::kOff);
  {
    PerformanceSettings settings(PerformanceConfig{});
    CHECK(GetPerformanceMode(PerformanceStage::kInput) ==
          PerformanceMode::kOff);
    StageLatency latency;
    latency.Start(PerformanceStage::kInput, &latency);
    CHECK_FALSE(latency.enabled());
  }
  {
    PerformanceSettings settings(Config(PerformanceMode::kDetail));
    CHECK(GetPerformanceMode(PerformanceStage::kInput) ==
          PerformanceMode::kDetail);
    CHECK(GetPerformanceMode(PerformanceStage::kEncoderVideo) ==
          PerformanceMode::kOff);
    StageLatency latency;
    latency.Start(PerformanceStage::kEncoderVideo, &latency);
    CHECK_FALSE(latency.enabled());
  }
  CHECK(GetPerformanceMode(PerformanceStage::kInput) == PerformanceMode::kOff);
}

TEST_CASE("性能配置初始化时复制且结束后允许重新初始化", "[latency][unit]") {
  auto config = Config();
  {
    PerformanceSettings settings(config);
    config.stages[PerformanceStage::kInput] = PerformanceMode::kOff;
    config.stages[PerformanceStage::kEncoderVideo] = PerformanceMode::kDetail;
    CHECK(GetPerformanceMode(PerformanceStage::kInput) ==
          PerformanceMode::kSummary);
    CHECK(GetPerformanceMode(PerformanceStage::kEncoderVideo) ==
          PerformanceMode::kOff);
    REQUIRE_THROWS_AS(PerformanceSettings(config), std::logic_error);
    CHECK(GetPerformanceMode(PerformanceStage::kInput) ==
          PerformanceMode::kSummary);
  }
  CHECK(GetPerformanceMode(PerformanceStage::kInput) == PerformanceMode::kOff);
  PerformanceSettings settings(config);
  CHECK(GetPerformanceMode(PerformanceStage::kInput) == PerformanceMode::kOff);
  CHECK(GetPerformanceMode(PerformanceStage::kEncoderVideo) ==
        PerformanceMode::kDetail);
}

TEST_CASE("性能配置拒绝非法阶段和档位且不发布失败实例", "[latency][unit]") {
  PerformanceConfig config;
  SECTION("非法阶段") {
    config.stages[static_cast<PerformanceStage>(255)] =
        PerformanceMode::kSummary;
  }
  SECTION("非法档位") {
    config.stages[PerformanceStage::kInput] = static_cast<PerformanceMode>(255);
  }
  REQUIRE_THROWS_AS(PerformanceSettings(config), std::invalid_argument);
  CHECK(GetPerformanceMode(PerformanceStage::kInput) == PerformanceMode::kOff);
  PerformanceSettings settings(Config());
  CHECK(GetPerformanceMode(PerformanceStage::kInput) ==
        PerformanceMode::kSummary);
}

TEST_CASE("日志模块级别不能开启性能采集", "[latency][unit]") {
  auto log_config = LogConfig();
  log_config.modules["perf.input"] = mw::log::LogLevel::kTrace;
  mw::log::Logging logging(log_config);
  PerformanceSettings settings(PerformanceConfig{});
  StageLatency latency;
  latency.Start(PerformanceStage::kInput, &latency);
  CHECK_FALSE(latency.enabled());
  latency.Observe(1000000);
  CHECK(latency.samples().valid == 0);
}

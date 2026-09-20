#include <future>
#include <memory>
#include <vector>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "SrtEpollReactor.h"
#include "mw/streamer/pipeline/internal/pipeline_builder.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace mw::streamer;

PipelineConfig RecordingConfig() {
  PipelineConfig config;
  config.input.options.url = "rtsp://127.0.0.1/live/test";
  config.input.downstream = {"record"};
  auto sink = std::make_unique<RemuxNodeConfig>("record");
  sink->options.target = "runtime-test.mp4";
  config.sinks.push_back(std::move(sink));
  return config;
}

internal::RuntimeConfig RuntimeConfig() {
  internal::RuntimeConfig config;
  config.zlm.event_poller_threads = 1;
  config.zlm.work_threads = 1;
  config.zlm.enable_cpu_affinity = false;
  return config;
}

void CheckReleased() {
  CHECK_FALSE(toolkit::EventPollerPool::isCreated());
  CHECK_FALSE(toolkit::WorkThreadPool::isCreated());
  CHECK_FALSE(mediakit::SrtEpollReactor::isCreated());
}

TEST_CASE("Last pipeline releases the SRT reactor and permits recreation",
          "[runtime][lifecycle][srt]") {
  CheckReleased();
  for (int iteration = 0; iteration < 3; ++iteration) {
    {
      auto pipeline = internal::BuildPipelineWithRuntime(
          RecordingConfig(), {}, RuntimeConfig());
      REQUIRE(mediakit::SrtEpollReactor::Instance().available());
      REQUIRE(mediakit::SrtEpollReactor::isCreated());
    }
    CheckReleased();
  }
}

TEST_CASE("Last pipeline releases shared pools and permits recreation",
          "[runtime][lifecycle]") {
  CheckReleased();
  for (int iteration = 0; iteration < 3; ++iteration) {
    auto first = internal::BuildPipelineWithRuntime(RecordingConfig(), {}, RuntimeConfig());
    auto second = internal::BuildPipelineWithRuntime(RecordingConfig(), {}, RuntimeConfig());
    REQUIRE(toolkit::EventPollerPool::isCreated());
    std::weak_ptr<toolkit::EventPoller> poller =
        toolkit::EventPollerPool::Instance().getPoller();
    std::weak_ptr<toolkit::EventPoller> worker =
        toolkit::WorkThreadPool::Instance().getPoller();

    first.reset();
    CHECK(toolkit::EventPollerPool::isCreated());
    CHECK(toolkit::WorkThreadPool::isCreated());
    CHECK_FALSE(poller.expired());
    CHECK_FALSE(worker.expired());

    second.reset();
    CheckReleased();
    CHECK(poller.expired());
    CHECK(worker.expired());
  }
}

TEST_CASE("Failed pipeline build does not strand a runtime owner",
          "[runtime][lifecycle]") {
  CheckReleased();
  auto config = RecordingConfig();
  ProcessorBindings invalid;
  invalid.analysis.emplace("missing", MwStreamerAnalysisProcessorCallbacks{});
  CHECK_THROWS(internal::BuildPipelineWithRuntime(config, invalid, RuntimeConfig()));
  CheckReleased();

  auto survivor = internal::BuildPipelineWithRuntime(config, {}, RuntimeConfig());
  CHECK_THROWS(internal::BuildPipelineWithRuntime(config, invalid, RuntimeConfig()));
  CHECK(toolkit::EventPollerPool::isCreated());
  survivor.reset();
  CheckReleased();
}

TEST_CASE("Concurrent pipeline creation and destruction release all pools",
          "[runtime][lifecycle]") {
  CheckReleased();
  for (int iteration = 0; iteration < 3; ++iteration) {
    std::promise<void> ready;
    auto start = ready.get_future().share();
    std::vector<std::future<void>> tasks;
    for (int thread = 0; thread < 4; ++thread) {
      tasks.push_back(std::async(std::launch::async, [start] {
        start.wait();
        for (int index = 0; index < 3; ++index) {
          auto pipeline = internal::BuildPipelineWithRuntime(
              RecordingConfig(), {}, RuntimeConfig());
        }
      }));
    }
    ready.set_value();
    for (auto& task : tasks) REQUIRE_NOTHROW(task.get());
    CheckReleased();
  }
}

}  // namespace

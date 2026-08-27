#include "mw/pipeline/internal/streaming/output_event_mailbox.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mw/processor/streaming_processor_handler.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::pipeline::internal::streaming::OutputEventMailbox;
using mw::streamer::pipeline::internal::streaming::OutputEventSubmitResult;
using mw::streamer::processor::StreamingProcessorHandler;

MwStreamerProcessorSourceInfo MakeSourceInfo() {
  MwStreamerProcessorSourceInfo source_info{};
  source_info.has_audio = 1;
  source_info.audio.sample_rate = 48000;
  source_info.audio.channel_count = 2;
  source_info.audio.time_base = {1, 48000};
  return source_info;
}

struct CallbackState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::string> sink_ids;
  std::vector<std::string> types;
  std::vector<std::vector<std::uint8_t>> payloads;
  std::vector<MwStreamerMediaTimestamp> timestamps;
  std::thread::id callback_thread;
  bool block_first = false;
  bool first_entered = false;
  bool release_first = false;
};

void OnOutputEvent(const MwStreamerOutputEvent* event, void* user_context) {
  auto& state = *static_cast<CallbackState*>(user_context);
  std::unique_lock<std::mutex> lock(state.mutex);
  state.callback_thread = std::this_thread::get_id();
  state.sink_ids.emplace_back(event->sink_id);
  state.types.emplace_back(event->type);
  const auto* payload = static_cast<const std::uint8_t*>(event->payload);
  if (event->payload_size == 0) {
    state.payloads.emplace_back();
  } else {
    state.payloads.emplace_back(payload, payload + event->payload_size);
  }
  state.timestamps.push_back(event->timestamp);
  if (state.block_first && state.sink_ids.size() == 1) {
    state.first_entered = true;
    state.condition.notify_all();
    state.condition.wait(lock, [&state]() { return state.release_first; });
  }
  state.condition.notify_all();
}

std::unique_ptr<StreamingProcessorHandler> MakeStartedProcessor(
    CallbackState& state) {
  auto processor =
      std::make_unique<StreamingProcessorHandler>(MakeSourceInfo(), nullptr);
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_output_event = OnOutputEvent;
  const MwStreamerStreamingProcessorConfig config = {0, 0, ""};
  REQUIRE(processor->Start(config, callbacks) ==
          kMwStreamerProcessorStartSuccess);
  return processor;
}

MwStreamerOutputEvent MakeEvent(const char* sink_id, const char* type,
                                const void* payload, std::size_t payload_size,
                                std::int64_t pts) {
  return {
      sink_id, type, payload, payload_size, 1, {pts, 0, {1, 1000}},
  };
}

TEST_CASE("OutputEventMailbox深拷贝事件并按提交顺序异步投递") {
  CallbackState state;
  auto processor = MakeStartedProcessor(state);
  OutputEventMailbox mailbox(4, *processor);
  mailbox.Start();
  const auto caller_thread = std::this_thread::get_id();

  std::string sink_id = "raw-output";
  std::string type = "pointer-down";
  std::vector<std::uint8_t> payload = {1, 2, 3};
  REQUIRE(mailbox.Submit(MakeEvent(sink_id.c_str(), type.c_str(),
                                   payload.data(), payload.size(), 10)) ==
          OutputEventSubmitResult::kAccepted);
  sink_id = "changed";
  type = "changed";
  payload.assign({9, 9, 9});

  const std::uint8_t second_payload[] = {4, 5};
  REQUIRE(mailbox.Submit(MakeEvent("preview", "resize", second_payload,
                                   sizeof(second_payload), 20)) ==
          OutputEventSubmitResult::kAccepted);

  {
    std::unique_lock<std::mutex> lock(state.mutex);
    REQUIRE(state.condition.wait_for(
        lock, 1s, [&state]() { return state.sink_ids.size() == 2; }));
    CHECK(state.sink_ids == std::vector<std::string>{"raw-output", "preview"});
    CHECK(state.types == std::vector<std::string>{"pointer-down", "resize"});
    CHECK(state.payloads[0] == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(state.payloads[1] == std::vector<std::uint8_t>{4, 5});
    CHECK(state.timestamps[0].pts == 10);
    CHECK(state.timestamps[1].pts == 20);
    CHECK(state.callback_thread != caller_thread);
  }

  mailbox.Stop();
  CHECK(mailbox.Submit(MakeEvent("raw", "late", nullptr, 0, 30)) ==
        OutputEventSubmitResult::kStopped);
  processor->Stop();
}

TEST_CASE("OutputEventMailbox队列满时非阻塞拒绝并在停止时丢弃积压") {
  CallbackState state;
  state.block_first = true;
  auto processor = MakeStartedProcessor(state);
  OutputEventMailbox mailbox(1, *processor);
  mailbox.Start();

  REQUIRE(mailbox.Submit(MakeEvent("raw", "first", nullptr, 0, 1)) ==
          OutputEventSubmitResult::kAccepted);
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    REQUIRE(state.condition.wait_for(
        lock, 1s, [&state]() { return state.first_entered; }));
  }
  REQUIRE(mailbox.Submit(MakeEvent("raw", "queued", nullptr, 0, 2)) ==
          OutputEventSubmitResult::kAccepted);
  CHECK(mailbox.Submit(MakeEvent("raw", "full", nullptr, 0, 3)) ==
        OutputEventSubmitResult::kQueueFull);

  mailbox.RequestStop();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.release_first = true;
  }
  state.condition.notify_all();
  mailbox.Stop();

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    REQUIRE(state.types.size() == 1);
    CHECK(state.types.front() == "first");
  }
  processor->Stop();
}

TEST_CASE("OutputEventMailbox校验事件和生命周期边界") {
  CallbackState state;
  auto processor = MakeStartedProcessor(state);
  CHECK_THROWS_AS(OutputEventMailbox(0, *processor), std::invalid_argument);

  OutputEventMailbox mailbox(1, *processor);
  CHECK(mailbox.Submit(MakeEvent("raw", "early", nullptr, 0, 1)) ==
        OutputEventSubmitResult::kStopped);
  mailbox.Start();
  CHECK_THROWS_AS(mailbox.Start(), std::logic_error);

  CHECK_THROWS_AS(mailbox.Submit(MakeEvent(nullptr, "type", nullptr, 0, 1)),
                  std::invalid_argument);
  CHECK_THROWS_AS(mailbox.Submit(MakeEvent("raw", nullptr, nullptr, 0, 1)),
                  std::invalid_argument);
  CHECK_THROWS_AS(mailbox.Submit(MakeEvent("raw", "type", nullptr, 1, 1)),
                  std::invalid_argument);

  mailbox.Stop();
  mailbox.Stop();
  CHECK(mailbox.Submit(MakeEvent("raw", "late", nullptr, 0, 1)) ==
        OutputEventSubmitResult::kStopped);
  processor->Stop();
}

}  // namespace

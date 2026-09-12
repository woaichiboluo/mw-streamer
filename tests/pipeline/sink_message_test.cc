#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/streamer/pipeline/pipeline.h"
#include "mw/streamer/processor/analysis_processor_sink.h"
#include "mw/streamer/processor/transform_processor_sink.h"
#include "mw/streamer/sink/fatal_error.h"
#include "mw/streamer/sink/sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::Input;
using mw::streamer::InputState;
using mw::streamer::FrameReady;
using mw::streamer::FrameStreamsReady;
using mw::streamer::PacketReady;
using mw::streamer::StreamsReady;
using mw::streamer::AnalysisProcessorSink;
using mw::streamer::TransformProcessorSink;
using mw::streamer::FatalError;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using mw::streamer::SinkMessage;
using namespace mw::streamer;
using mw::streamer::Frame;
using mw::streamer::StreamInfo;

FrameStreamsReady Streams() {
  StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = {1, 90000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->width = 64;
  parameters->height = 32;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->framerate = {25, 1};
  return {1, {std::move(stream)}, nullptr};
}

Frame Video() {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 64;
  frame->height = 32;
  frame->pts = 9000;
  frame->time_base = {1, 90000};
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  for (int plane = 0; plane < 3; ++plane) {
    std::memset(frame->data[plane], 0x30,
                static_cast<std::size_t>(plane == 0 ? 32 : 16) *
                    frame->linesize[plane]);
  }
  return frame;
}

struct RecordedMessage {
  std::string sink_id;
  std::string type;
  std::string payload;
  std::optional<MwStreamerMediaTimestamp> timestamp;
  std::thread::id thread;
};

RecordedMessage CopyMessage(const SinkMessage& message) {
  RecordedMessage owned;
  owned.sink_id = message.sink_id;
  owned.type = message.type;
  if (message.payload_size != 0) {
    owned.payload.assign(static_cast<const char*>(message.payload),
                         message.payload_size);
  }
  owned.timestamp = message.timestamp;
  owned.thread = std::this_thread::get_id();
  return owned;
}

struct MessageState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<RecordedMessage> messages;
  std::shared_future<void> release;
  bool active = false;
  bool stop_overlap = false;
  int stops = 0;

  bool WaitActive() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 2s, [&]() { return active; });
  }

  bool WaitMessages(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 2s,
                            [&]() { return messages.size() >= count; });
  }

  void Receive(const SinkMessage& message) {
    const bool block = message.type == "block";
    const bool fail = message.type == "throw";
    const bool fatal = message.type == "fatal";
    std::unique_lock<std::mutex> lock(mutex);
    active = true;
    changed.notify_all();
    if (block) {
      lock.unlock();
      release.wait();
      lock.lock();
    }
    messages.push_back(CopyMessage(message));
    active = false;
    changed.notify_all();
    lock.unlock();
    if (fatal) {
      throw FatalError("fatal message failure");
    }
    if (fail) {
      throw std::runtime_error("ordinary message failure");
    }
  }

  void Stopped() {
    std::lock_guard<std::mutex> lock(mutex);
    stop_overlap |= active;
    ++stops;
    changed.notify_all();
  }
};

// Declare after the sink so unwinding releases a callback before destruction.
class ReleaseGuard {
 public:
  explicit ReleaseGuard(MessageState& state) {
    state.release = promise_.get_future().share();
  }
  ~ReleaseGuard() { Release(); }
  void Release() {
    if (!released_) {
      promise_.set_value();
      released_ = true;
    }
  }

 private:
  std::promise<void> promise_;
  bool released_ = false;
};

// Futures join on destruction; release their callback first during unwinding.
struct ReleaseBeforeJoin {
  ReleaseGuard& guard;
  ~ReleaseBeforeJoin() { guard.Release(); }
};

void ReceiveMessage(const MwStreamerMessage* message, void* context) {
  static_cast<MessageState*>(context)->Receive(
      {message->sink_id, message->type, message->payload, message->payload_size,
       message->has_timestamp
           ? std::optional<MwStreamerMediaTimestamp>(message->timestamp)
           : std::nullopt});
}

MwStreamerTransformProcessorCallbacks Callbacks(MessageState& state) {
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerTransformProcessorStartRequest*,
                          void*) { return kMwStreamerProcessorStartSuccess; };
  callbacks.on_message = ReceiveMessage;
  callbacks.on_stop = [](void* context) {
    static_cast<MessageState*>(context)->Stopped();
  };
  return callbacks;
}

class MessageSink final : public Sink {
 public:
  explicit MessageSink(std::string id, MessageState* state = nullptr)
      : Sink(std::move(id), SinkMediaType::kFrame, SinkMediaType::kFrame),
        state_(state) {}
  ~MessageSink() override { Stop(); }

  void Emit(const SinkMessage& message) { SendMessage(message); }
  void OnStreamsReady(const FrameStreamsReady& streams) override {
    CloseRegistration();
    StartMessages();
    SendStreamsReady(streams);
  }
  void OnAudioFrame(const FrameReady&) override {}
  void OnVideoFrame(const FrameReady&) override {
    if (send_on_video) {
      Emit({"media", "frame"});
    }
  }
  void Stop() noexcept override {
    StopMessages();
    if (!stopped_.exchange(true)) {
      if (send_on_stop) {
        Emit({"child", "during-stop"});
      }
      StopDownstream();
      if (state_) {
        state_->Stopped();
      }
    }
  }

  MessageState* observed_default = nullptr;
  bool send_on_video = false;
  bool send_on_stop = false;

 protected:
  void OnMessage(const SinkMessage& message) override {
    if (state_) {
      state_->Receive(message);
    } else {
      Sink::OnMessage(message);
      if (observed_default) {
        observed_default->Receive(message);
      }
    }
  }

 private:
  MessageState* state_;
  std::atomic<bool> stopped_{false};
};

class TestInput final : public Input {
 public:
  void Start(Observer& observer) override {
    state_.store(InputState::kReady);
    observer.OnStreamsReady({1, Streams().source_streams});
  }
  void Stop() noexcept override { state_.store(InputState::kStopped); }
  InputState state() const noexcept override { return state_.load(); }

 private:
  std::atomic<InputState> state_{InputState::kIdle};
};

class FrameSource final : public Sink {
 public:
  FrameSource()
      : Sink("source", SinkMediaType::kPacket, SinkMediaType::kFrame) {}
  ~FrameSource() override { Stop(); }
  void OnStreamsReady(const StreamsReady&) override {
    CloseRegistration();
    StartMessages();
    SendStreamsReady(Streams());
  }
  void OnPacket(const PacketReady&) override {}
};

class MessageGraph {
 public:
  MessageGraph() : pipeline(std::make_unique<TestInput>()) {
    auto source = std::make_unique<FrameSource>();
    root_ = source.get();
    pipeline.AddSink(std::move(source));
  }
  MessageSink& Add(const std::string& id, MessageState* state = nullptr) {
    auto sink = std::make_unique<MessageSink>(id, state);
    auto& result = *sink;
    root_->AddSink(std::move(sink));
    return result;
  }
  void Add(std::unique_ptr<Sink> sink) { root_->AddSink(std::move(sink)); }
  void Bind(const std::string& sender, const std::string& receiver) {
    pipeline.SetMessageReceiver(sender, receiver);
  }

  Pipeline pipeline;

 private:
  FrameSource* root_;
};

}  // namespace

TEST_CASE("Pipeline按目标ID绕过中间媒体节点直达Processor") {
  MessageState state;
  MessageGraph graph;
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", Callbacks(state));
  auto middle = std::make_unique<MessageSink>("middle");
  auto leaf = std::make_unique<MessageSink>("leaf");
  auto* sender = leaf.get();
  middle->AddSink(std::move(leaf));
  processor->AddSink(std::move(middle));
  graph.Add(std::move(processor));
  graph.Bind("leaf", "processor");
  graph.pipeline.Start();
  sender->Emit({"leaf", "mark"});
  const bool delivered = state.WaitMessages(1);
  graph.pipeline.Stop();
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 1);
  CHECK(state.messages[0].sink_id == "leaf");
  CHECK(state.messages[0].type == "mark");
}

TEST_CASE("Pipeline跨分支绑定且默认OnMessage不自动转发") {
  MessageState destination_state;
  MessageState observed;
  MessageGraph graph;
  graph.Add("destination", &destination_state);
  auto& receiver = graph.Add("receiver");
  receiver.observed_default = &observed;
  auto& sender = graph.Add("sender");
  graph.Bind("receiver", "destination");
  graph.Bind("sender", "receiver");
  graph.pipeline.Start();
  sender.Emit({"sender", "ignored"});
  const bool handled = observed.WaitMessages(1);
  graph.pipeline.Stop();
  REQUIRE(handled);
  CHECK(destination_state.messages.empty());
}

TEST_CASE("媒体连接不隐式绑定消息接收者且生命周期外发送静默忽略") {
  MessageState state;
  MessageGraph graph;
  auto& receiver = graph.Add("receiver", &state);
  auto& unbound = graph.Add("unbound");
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "receiver");
  sender.Emit({"sender", "before"});
  graph.pipeline.Start();
  unbound.Emit({"unbound", "unbound"});
  receiver.Emit({"receiver", "unbound"});
  sender.Emit({"sender", "running"});
  const bool handled = state.WaitMessages(1);
  graph.pipeline.Stop();
  sender.Emit({"sender", "after"});
  REQUIRE(handled);
  REQUIRE(state.messages.size() == 1);
  CHECK(state.messages[0].type == "running");
}

TEST_CASE("Pipeline启动验证消息目标发送方和重复ID") {
  MessageGraph graph;
  graph.Add("receiver");
  graph.Add("sender");
  SECTION("不存在的接收者") { graph.Bind("sender", "missing"); }
  SECTION("不存在的发送者") { graph.Bind("missing", "receiver"); }
  SECTION("重复节点ID") { graph.Add("receiver"); }
  CHECK_THROWS_AS(graph.pipeline.Start(), std::invalid_argument);
}

TEST_CASE("Pipeline消息深拷贝并保持多发送者的入队顺序") {
  MessageState state;
  MessageGraph graph;
  ReleaseGuard release(state);
  graph.Add("receiver", &state);
  auto& first = graph.Add("first");
  auto& second = graph.Add("second");
  graph.Bind("first", "receiver");
  graph.Bind("second", "receiver");
  graph.pipeline.Start();
  const auto producer = std::this_thread::get_id();
  first.Emit({"first", "block"});
  const bool entered = state.WaitActive();
  std::string id = "local-source";
  std::string type = "local-type";
  std::string payload("a\0b", 3);
  MwStreamerMediaTimestamp timestamp{123, 7, {1, 90000}};
  first.Emit({id, type, payload.data(), payload.size(), timestamp});
  id.assign("changed");
  type.assign("changed");
  payload.assign("xxx");
  timestamp.pts = 0;
  second.Emit({"second", "next"});
  CHECK_THROWS_AS(first.Emit({"first", "invalid", nullptr, 1}),
                  std::invalid_argument);
  release.Release();
  const bool delivered = state.WaitMessages(3);
  graph.pipeline.Stop();
  REQUIRE(entered);
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 3);
  CHECK(state.messages[0].type == "block");
  const auto& message = state.messages[1];
  CHECK(message.sink_id == "local-source");
  CHECK(message.type == "local-type");
  CHECK(message.payload == std::string("a\0b", 3));
  REQUIRE(message.timestamp.has_value());
  CHECK(message.timestamp->pts == 123);
  CHECK(message.timestamp->duration == 7);
  CHECK(message.timestamp->time_base.num == 1);
  CHECK(message.timestamp->time_base.den == 90000);
  CHECK(state.messages[2].sink_id == "second");
  CHECK_FALSE(state.messages[2].timestamp.has_value());
  for (const auto& event : state.messages) {
    CHECK(event.thread != producer);
    CHECK(event.thread == state.messages[0].thread);
  }
}

TEST_CASE("Pipeline所有接收者共用同一Poller并串行处理消息") {
  MessageState first_state;
  MessageState second_state;
  MessageGraph graph;
  ReleaseGuard release(first_state);
  graph.Add("first-receiver", &first_state);
  graph.Add("second-receiver", &second_state);
  auto& first = graph.Add("first-sender");
  auto& second = graph.Add("second-sender");
  graph.Bind("first-sender", "first-receiver");
  graph.Bind("second-sender", "second-receiver");
  graph.pipeline.Start();
  first.Emit({"first", "block"});
  const bool entered = first_state.WaitActive();
  second.Emit({"second", "next"});
  {
    std::unique_lock<std::mutex> lock(second_state.mutex);
    CHECK_FALSE(second_state.changed.wait_for(
        lock, 50ms, [&]() { return !second_state.messages.empty(); }));
  }
  release.Release();
  const bool first_delivered = first_state.WaitMessages(1);
  const bool second_delivered = second_state.WaitMessages(1);
  graph.pipeline.Stop();
  REQUIRE(entered);
  REQUIRE(first_delivered);
  REQUIRE(second_delivered);
  CHECK(first_state.messages[0].thread == second_state.messages[0].thread);
  CHECK(first_state.messages[0].thread != std::this_thread::get_id());
}

TEST_CASE("消息回调内再次发送仍异步执行且不会重入接收者") {
  struct State {
    MessageSink* sender = nullptr;
    std::vector<std::string> order;
    std::promise<void> done;
  } state;
  auto done = state.done.get_future();
  MessageGraph graph;
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_message = [](const MwStreamerMessage* message, void* context) {
    auto& state = *static_cast<State*>(context);
    if (std::string_view(message->type) == "first") {
      state.order.push_back("enter");
      state.sender->Emit({"sender", "second"});
      state.order.push_back("leave");
      return;
    }
    state.order.push_back("second");
    state.done.set_value();
  };
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", callbacks);
  auto sender = std::make_unique<MessageSink>("sender");
  state.sender = sender.get();
  processor->AddSink(std::move(sender));
  graph.Add(std::move(processor));
  graph.Bind("sender", "processor");
  graph.pipeline.Start();
  state.sender->Emit({"sender", "first"});
  const auto delivered = done.wait_for(2s);
  graph.pipeline.Stop();
  REQUIRE(delivered == std::future_status::ready);
  CHECK(state.order == std::vector<std::string>{"enter", "leave", "second"});
}

TEST_CASE("Pipeline消息投递不受旧256条容量限制") {
  MessageState state;
  MessageGraph graph;
  ReleaseGuard release(state);
  graph.Add("receiver", &state);
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "receiver");
  graph.pipeline.Start();
  sender.Emit({"test", "block"});
  const bool entered = state.WaitActive();
  for (int index = 0; index < 1024; ++index) {
    const std::string payload = std::to_string(index);
    sender.Emit({"test", "queued", payload.data(), payload.size()});
  }
  release.Release();
  const bool delivered = state.WaitMessages(1025);
  graph.pipeline.Stop();
  REQUIRE(entered);
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 1025);
  for (int index = 0; index < 1024; ++index) {
    CHECK(state.messages[index + 1].payload == std::to_string(index));
  }
}

TEST_CASE("Pipeline支持并发发送且保持各发送线程的消息顺序") {
  MessageState state;
  MessageGraph graph;
  graph.Add("receiver", &state);
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "receiver");
  graph.pipeline.Start();
  std::vector<std::future<void>> producers;
  for (int producer = 0; producer < 4; ++producer) {
    producers.push_back(std::async(std::launch::async, [&, producer]() {
      const std::string id = std::to_string(producer);
      for (int index = 0; index < 64; ++index) {
        const std::string payload = std::to_string(index);
        sender.Emit({id, "concurrent", payload.data(), payload.size()});
      }
    }));
  }
  for (auto& producer : producers) {
    producer.get();
  }
  const bool delivered = state.WaitMessages(256);
  graph.pipeline.Stop();
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 256);
  int next[4] = {};
  for (const auto& message : state.messages) {
    const int producer = std::stoi(message.sink_id);
    REQUIRE(producer >= 0);
    REQUIRE(producer < 4);
    CHECK(message.payload == std::to_string(next[producer]++));
  }
  for (const int count : next) {
    CHECK(count == 64);
  }
}

TEST_CASE("Processor消息回调与媒体并发且媒体发送不等待消息处理") {
  MessageState state;
  MessageGraph graph;
  ReleaseGuard release(state);
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", Callbacks(state));
  auto* processor_sink = processor.get();
  auto child = std::make_unique<MessageSink>("sender");
  child->send_on_video = true;
  auto* sender = child.get();
  processor->AddSink(std::move(child));
  graph.Add(std::move(processor));
  graph.Bind("sender", "processor");
  graph.pipeline.Start();
  auto video = Video();
  sender->Emit({"test", "block"});
  const bool entered = state.WaitActive();
  auto delivery = std::async(std::launch::async, [&]() {
    processor_sink->OnVideoFrame({1, video});
  });
  ReleaseBeforeJoin release_before_join{release};
  const auto returned_while_blocked = delivery.wait_for(2s);
  release.Release();
  const auto finished = delivery.wait_for(2s);
  const bool delivered = state.WaitMessages(2);
  graph.pipeline.Stop();
  REQUIRE(entered);
  CHECK(returned_while_blocked == std::future_status::ready);
  REQUIRE(finished == std::future_status::ready);
  CHECK_NOTHROW(delivery.get());
  REQUIRE(delivered);
  CHECK(state.messages[1].type == "frame");
}

TEST_CASE("Pipeline隔离普通消息异常并继续处理后续消息") {
  MessageState state;
  MessageGraph graph;
  graph.Add("receiver", &state);
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "receiver");
  graph.pipeline.Start();
  sender.Emit({"test", "throw"});
  sender.Emit({"test", "next"});
  const bool delivered = state.WaitMessages(2);
  CHECK(graph.pipeline.state() == PipelineState::kRunning);
  graph.pipeline.Stop();
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 2);
  CHECK(state.messages[1].type == "next");
}

TEST_CASE("Pipeline消息Fatal沿接收方媒体树停止整条链路") {
  MessageState state;
  MessageGraph graph;
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", Callbacks(state));
  auto middle = std::make_unique<MessageSink>("middle");
  processor->AddSink(std::make_unique<MessageSink>("output"));
  middle->AddSink(std::move(processor));
  graph.Add(std::move(middle));
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "processor");
  graph.pipeline.Start();
  sender.Emit({"external", "fatal"});
  const bool delivered = state.WaitMessages(1);
  bool stopped;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    stopped =
        state.changed.wait_for(lock, 2s, [&]() { return state.stops > 0; });
  }
  graph.pipeline.Stop();
  REQUIRE(delivered);
  REQUIRE(stopped);
  CHECK(graph.pipeline.state() == PipelineState::kFailed);
  CHECK(graph.pipeline.error() == "fatal message failure");
  CHECK(state.stops == 1);
}

TEST_CASE("Pipeline停止丢弃积压并等待消息回调后才停止Processor") {
  MessageState state;
  MessageGraph graph;
  ReleaseGuard release(state);
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", Callbacks(state));
  processor->AddSink(std::make_unique<MessageSink>("output"));
  graph.Add(std::move(processor));
  auto& sender = graph.Add("sender");
  sender.send_on_stop = true;
  graph.Bind("sender", "processor");
  graph.pipeline.Start();
  sender.Emit({"test", "block"});
  const bool entered = state.WaitActive();
  sender.Emit({"test", "discard"});
  std::promise<void> stop_entered;
  auto stop_entered_future = stop_entered.get_future();
  auto stop = std::async(std::launch::async, [&]() {
    stop_entered.set_value();
    graph.pipeline.Stop();
  });
  ReleaseBeforeJoin release_before_join{release};
  const auto stop_started = stop_entered_future.wait_for(2s);
  const auto while_blocked = stop.wait_for(50ms);
  sender.Emit({"test", "late"});
  release.Release();
  const auto finished = stop.wait_for(2s);
  REQUIRE(entered);
  REQUIRE(stop_started == std::future_status::ready);
  CHECK(while_blocked == std::future_status::timeout);
  REQUIRE(finished == std::future_status::ready);
  CHECK_NOTHROW(stop.get());
  sender.Emit({"test", "closed"});
  REQUIRE(state.messages.size() == 1);
  CHECK(state.messages[0].type == "block");
  CHECK_FALSE(state.stop_overlap);
  CHECK(state.stops == 1);
  graph.pipeline.Stop();
  CHECK(state.stops == 1);
}

TEST_CASE("AnalysisProcessor通过Pipeline公共消息循环接收回调") {
  MessageState state;
  MessageGraph graph;
  MwStreamerAnalysisProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerAnalysisProcessorStartRequest*,
                          void*) { return kMwStreamerProcessorStartSuccess; };
  callbacks.on_message = ReceiveMessage;
  callbacks.on_stop = [](void* context) {
    static_cast<MessageState*>(context)->Stopped();
  };
  auto processor =
      std::make_unique<AnalysisProcessorSink>("analysis", callbacks);
  graph.Add(std::move(processor));
  auto& sender = graph.Add("sender");
  graph.Bind("sender", "analysis");
  sender.Emit({"external", "before"});
  graph.pipeline.Start();
  sender.Emit({"external", "analysis"});
  const bool delivered = state.WaitMessages(1);
  graph.pipeline.Stop();
  sender.Emit({"external", "after"});
  REQUIRE(delivered);
  REQUIRE(state.messages.size() == 1);
  CHECK(state.messages[0].type == "analysis");
  CHECK(state.stops == 1);
}

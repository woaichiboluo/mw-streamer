#include "mw/streamer/pipeline/pipeline.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/common/blocking_queue.h"
#include "mw/streamer/input/zlm_input.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::Input;
using mw::streamer::InputState;
using mw::streamer::InputStateChanged;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;
using mw::streamer::PacketReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;
using mw::streamer::TimelineResetReason;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using namespace mw::streamer;
using mw::streamer::BlockingQueue;

class FakeInput final : public Input {
 public:
  explicit FakeInput(std::vector<std::string>& trace) : trace_(trace) {}
  ~FakeInput() override { trace_.push_back("input destroyed"); }

  void Start(Observer& observer) override {
    trace_.push_back("input start");
    if (fail_start_) {
      throw std::invalid_argument("invalid source");
    }
    observer_ = &observer;
    state_ = InputState::kConnecting;
  }

  void Stop() noexcept override {
    trace_.push_back("input stop");
    observer_ = nullptr;
    state_ = InputState::kStopped;
  }

  InputState state() const noexcept override { return state_; }

  void SetStatus(const InputStateChanged& status) {
    state_ = status.state;
    observer_->OnInputStateChanged(status);
  }

  Observer* observer_ = nullptr;
  bool fail_start_ = false;

 private:
  std::vector<std::string>& trace_;
  InputState state_ = InputState::kIdle;
};

class RecordingSink final : public Sink {
 public:
  RecordingSink(std::string name, std::vector<std::string>& trace)
      : Sink(name, SinkMediaType::kPacket),
        name_(std::move(name)),
        trace_(trace) {}
  ~RecordingSink() override { trace_.push_back(name_ + " destroyed"); }

  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    trace_.push_back(name_ + " streams");
    generation_ = streams.generation;
  }
  void OnPacket(const PacketReady& packet) noexcept override {
    trace_.push_back(name_ + " packet");
    generation_ = packet.generation;
    packet_address_ = &packet;
  }
  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    trace_.push_back(name_ + " reset");
    generation_ = reset.generation;
    reset_reason_ = reset.reason;
    position_ = reset.position;
  }
  void OnInputEnded(const StreamEnded& end) noexcept override {
    trace_.push_back(name_ + " end");
    generation_ = end.generation;
    end_reason_ = end.reason;
  }
  void Stop() noexcept override { trace_.push_back(name_ + " stop"); }

  PacketSinkState state() const noexcept { return PacketSinkState::kIdle; }

  std::uint64_t generation_ = 0;
  const PacketReady* packet_address_ = nullptr;
  TimelineResetReason reset_reason_ = TimelineResetReason::kSeek;
  std::optional<std::chrono::milliseconds> position_;
  StreamEndReason end_reason_ = StreamEndReason::kEof;

 private:
  std::string name_;
  std::vector<std::string>& trace_;
};

class QueuedSink final : public Sink {
 public:
  explicit QueuedSink(std::string id)
      : Sink(std::move(id), SinkMediaType::kPacket) {}
  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    streams_ = streams;
    state_.store(PacketSinkState::kRunning);
  }
  void OnPacket(const PacketReady& packet) noexcept override {
    queue_.Push(packet);
  }
  void OnTimelineReset(const TimelineReset&) noexcept override {}
  void OnInputEnded(const StreamEnded& end) noexcept override {
    end_ = end;
    state_.store(PacketSinkState::kDraining);
    ended_.set_value();
  }
  void Stop() noexcept override {}
  PacketSinkState state() const noexcept { return state_.load(); }

  std::vector<PacketReady> Drain() {
    queue_.Close();
    std::vector<PacketReady> packets;
    while (auto packet = queue_.WaitPop()) {
      packets.push_back(std::move(*packet));
    }
    state_.store(PacketSinkState::kEnded);
    return packets;
  }

  BlockingQueue<PacketReady> queue_;
  StreamsReady streams_{};
  StreamEnded end_{};
  std::promise<void> ended_;

 private:
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
};

ZlmInputConfig SampleConfig() {
  ZlmInputConfig config;
  config.url = std::string(MW_PIPELINE_TEST_DATA_DIR) + "/h264_aac.mp4";
  config.reconnect_policy.max_retries = 0;
  return config;
}

std::uint64_t PacketHash(const PacketReady& packet) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (int i = 0; i < packet.packet->size; ++i) {
    hash = (hash ^ packet.packet->data[i]) * 1099511628211ULL;
  }
  return hash;
}

}  // namespace

TEST_CASE("pipeline synchronously fans named calls out in registration order") {
  std::vector<std::string> trace;
  auto input = std::make_unique<FakeInput>(trace);
  auto* source = input.get();
  Pipeline pipeline(std::move(input));
  auto first = std::make_unique<RecordingSink>("first", trace);
  auto second = std::make_unique<RecordingSink>("second", trace);
  auto* first_sink = first.get();
  auto* second_sink = second.get();
  pipeline.AddSink(std::move(first));
  pipeline.AddSink(std::move(second));
  pipeline.Start();
  trace.clear();

  source->observer_->OnStreamsReady({7, {}});
  const PacketReady packet{7, {}};
  source->observer_->OnPacket(packet);
  CHECK(first_sink->packet_address_ == &packet);
  CHECK(second_sink->packet_address_ == &packet);
  source->observer_->OnInputEnded({7, StreamEndReason::kInterrupted});
  CHECK(first_sink->end_reason_ == StreamEndReason::kInterrupted);
  CHECK(second_sink->end_reason_ == StreamEndReason::kInterrupted);
  source->observer_->OnTimelineReset(
      {9, TimelineResetReason::kReconnect, std::nullopt});
  CHECK(first_sink->generation_ == 9);
  CHECK(second_sink->generation_ == 9);
  CHECK(first_sink->reset_reason_ == TimelineResetReason::kReconnect);
  CHECK_FALSE(first_sink->position_.has_value());
  source->observer_->OnStreamsReady({9, {}});
  source->observer_->OnInputEnded({9, StreamEndReason::kEof});

  CHECK(trace == std::vector<std::string>{
                     "first streams", "second streams", "first packet",
                     "second packet", "first end", "second end", "first reset",
                     "second reset", "first streams", "second streams",
                     "first end", "second end"});
  CHECK(first_sink->end_reason_ == StreamEndReason::kEof);
  CHECK(second_sink->end_reason_ == StreamEndReason::kEof);
}

TEST_CASE("pipeline retains input status separately from sink notifications") {
  std::vector<std::string> trace;
  auto input = std::make_unique<FakeInput>(trace);
  auto* source = input.get();
  Pipeline pipeline(std::move(input));
  pipeline.AddSink(std::make_unique<RecordingSink>("sink", trace));
  CHECK(pipeline.input_status().state == InputState::kIdle);
  pipeline.Start();
  trace.clear();
  source->SetStatus({12, InputState::kWaitingRetry, "connection lost", true});
  auto snapshot = pipeline.input_status();
  CHECK(snapshot.generation == 12);
  CHECK(snapshot.state == InputState::kWaitingRetry);
  CHECK(snapshot.error == "connection lost");
  CHECK(snapshot.will_retry);
  snapshot.error.clear();
  CHECK(pipeline.input_status().error == "connection lost");
  source->SetStatus({13, InputState::kFailed, "retry exhausted", false});
  CHECK(pipeline.input_status().state == InputState::kFailed);
  CHECK_FALSE(pipeline.input_status().will_retry);
  CHECK(trace.empty());
}

TEST_CASE("pipeline validates ownership and enforces a single start attempt") {
  std::vector<std::string> trace;
  CHECK_THROWS_AS(Pipeline(nullptr), std::invalid_argument);
  auto input = std::make_unique<FakeInput>(trace);
  auto* source = input.get();
  Pipeline pipeline(std::move(input));
  CHECK_THROWS_AS(pipeline.AddSink(nullptr), std::invalid_argument);
  CHECK_THROWS_AS(pipeline.Start(), std::logic_error);
  CHECK(trace.empty());
  pipeline.AddSink(std::make_unique<RecordingSink>("sink", trace));

  SECTION("successful start closes registration and cannot repeat") {
    pipeline.Start();
    CHECK_THROWS_AS(pipeline.Start(), std::logic_error);
    CHECK_THROWS_AS(
        pipeline.AddSink(std::make_unique<RecordingSink>("late", trace)),
        std::logic_error);
    pipeline.Stop();
    const auto stopped_trace = trace;
    pipeline.Stop();
    CHECK(trace == stopped_trace);
    CHECK_THROWS_AS(pipeline.Start(), std::logic_error);
  }
  SECTION("stop before start closes the pipeline") {
    pipeline.Stop();
    CHECK_THROWS_AS(pipeline.Start(), std::logic_error);
    CHECK_THROWS_AS(
        pipeline.AddSink(std::make_unique<RecordingSink>("late", trace)),
        std::logic_error);
  }
  SECTION("a source start exception stops input and prevents restart") {
    source->fail_start_ = true;
    CHECK_THROWS_AS(pipeline.Start(), std::invalid_argument);
    CHECK(trace ==
          std::vector<std::string>{"input start", "input stop", "sink stop"});
    CHECK_THROWS_AS(pipeline.Start(), std::logic_error);
  }
}

TEST_CASE("pipeline destroys input before its exclusively owned sinks") {
  std::vector<std::string> trace;
  {
    Pipeline pipeline(std::make_unique<FakeInput>(trace));
    pipeline.AddSink(std::make_unique<RecordingSink>("sink", trace));
    pipeline.Start();
    trace.clear();
  }
  CHECK(trace == std::vector<std::string>{"input stop", "sink stop",
                                          "input destroyed", "sink destroyed"});
}

TEST_CASE("pipeline connects a real input to independent sink packet queues") {
  auto pipeline =
      std::make_unique<Pipeline>(std::make_unique<ZlmInput>(SampleConfig()));
  auto first = std::make_unique<QueuedSink>("first");
  auto second = std::make_unique<QueuedSink>("second");
  auto* first_sink = first.get();
  auto* second_sink = second.get();
  auto first_ended = first->ended_.get_future();
  auto second_ended = second->ended_.get_future();
  pipeline->AddSink(std::move(first));
  pipeline->AddSink(std::move(second));
  pipeline->Start();
  const auto first_result = first_ended.wait_for(5s);
  const auto second_result = second_ended.wait_for(5s);
  pipeline->Stop();

  REQUIRE(first_result == std::future_status::ready);
  REQUIRE(second_result == std::future_status::ready);
  CHECK(first_sink->state() == PacketSinkState::kDraining);
  CHECK(second_sink->state() == PacketSinkState::kDraining);
  CHECK(first_sink->end_.reason == StreamEndReason::kEof);
  CHECK(second_sink->end_.reason == StreamEndReason::kEof);
  REQUIRE(first_sink->streams_.streams.size() == 2);
  CHECK(first_sink->queue_.size() == 115);
  CHECK(second_sink->queue_.size() == 115);
  const auto first_packets = first_sink->Drain();
  CHECK(second_sink->queue_.size() == 115);
  CHECK(second_sink->state() == PacketSinkState::kDraining);
  const auto second_packets = second_sink->Drain();
  REQUIRE(first_packets.size() == 115);
  REQUIRE(second_packets.size() == first_packets.size());
  std::vector<std::uint64_t> hashes;
  for (const auto& packet : first_packets) {
    hashes.push_back(PacketHash(packet));
  }
  pipeline.reset();

  for (std::size_t i = 0; i < first_packets.size(); ++i) {
    REQUIRE(first_packets[i].packet->size > 0);
    CHECK(first_packets[i].packet->data == second_packets[i].packet->data);
    CHECK(PacketHash(first_packets[i]) == hashes[i]);
    CHECK(PacketHash(second_packets[i]) == hashes[i]);
  }
}

TEST_CASE("pipeline stop waits for a sink callback and leaves sinks alive") {
  class BlockingSink final : public Sink {
   public:
    explicit BlockingSink(std::shared_future<void> release)
        : Sink("blocking-sink", SinkMediaType::kPacket),
          release_(std::move(release)) {}
    void OnStreamsReady(const StreamsReady&) noexcept override {}
    void OnPacket(const PacketReady&) noexcept override {
      if (!blocked_) {
        blocked_ = true;
        entered_.set_value();
        release_.wait();
      }
      ++packets_;
    }
    void OnTimelineReset(const TimelineReset&) noexcept override {}
    void OnInputEnded(const StreamEnded&) noexcept override {}
    void Stop() noexcept override {}
    PacketSinkState state() const noexcept { return PacketSinkState::kRunning; }
    std::promise<void> entered_;
    std::atomic<std::size_t> packets_{0};

   private:
    std::shared_future<void> release_;
    bool blocked_ = false;
  };

  std::promise<void> release;
  auto sink = std::make_unique<BlockingSink>(release.get_future().share());
  auto* consumer = sink.get();
  auto entered = sink->entered_.get_future();
  Pipeline pipeline(std::make_unique<ZlmInput>(SampleConfig()));
  pipeline.AddSink(std::move(sink));
  pipeline.Start();
  const auto entered_result = entered.wait_for(5s);
  std::promise<void> stop_started;
  auto started = stop_started.get_future();
  auto stopped = std::async(std::launch::async, [&]() {
    stop_started.set_value();
    pipeline.Stop();
  });
  const auto started_result = started.wait_for(5s);
  const auto blocked_result = stopped.wait_for(100ms);
  auto performance =
      std::async(std::launch::async, [&] { return pipeline.GetPerformance(); });
  const auto performance_result = performance.wait_for(1s);
  // Release the source even if a timeout occurred, before any assertion.
  release.set_value();
  const auto stopped_result = stopped.wait_for(5s);
  REQUIRE(entered_result == std::future_status::ready);
  REQUIRE(started_result == std::future_status::ready);
  CHECK(blocked_result == std::future_status::timeout);
  CHECK(performance_result == std::future_status::ready);
  const auto snapshot = performance.get();
  const auto input_stats =
      snapshot.Find(mw::streamer::PerformanceType::kInput);
  REQUIRE(input_stats.size() == 1);
  CHECK(input_stats[0].operation->output_count > 0);
  CHECK(snapshot.sinks.size() == 1);
  REQUIRE(stopped_result == std::future_status::ready);
  stopped.get();
  const auto packets = consumer->packets_.load();
  pipeline.Stop();
  CHECK(consumer->packets_.load() == packets);
  CHECK(packets > 0);
}

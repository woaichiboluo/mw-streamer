#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/streamer/common/blocking_queue.h"
#include "mw/streamer/input/zlm_input.h"
#include "mw/streamer/sink/sink.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::BlockingQueue;
using mw::streamer::Packet;
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
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;

ZlmInputConfig SampleConfig() {
  ZlmInputConfig config;
  config.url = std::string(MW_PIPELINE_INPUT_TEST_DATA_DIR) + "/h264_aac.mp4";
  config.reconnect_policy.max_retries = 0;
  return config;
}

std::uint64_t PacketHash(const Packet& packet) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (int i = 0; i < packet->size; ++i) {
    hash = (hash ^ packet->data[i]) * 1099511628211ULL;
  }
  return hash;
}

struct BorrowedPacket {
  const std::uint8_t* data;
  std::uint64_t hash;
};

enum class CallKind { kStreams, kPacket, kReset, kEnd, kState };

// This is only an observation log, not the sink's queue or business protocol.
struct ObservedCall {
  CallKind kind;
  std::uint64_t generation;
};

// This consumer deliberately waits until the source is destroyed to process
// its packet queue, separating input EOF from sink completion.
class QueuedSink final : public Sink {
 public:
  explicit QueuedSink(std::string id)
      : Sink(std::move(id), SinkMediaType::kPacket) {}
  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    calls_.push_back({CallKind::kStreams, streams.generation});
    streams_.push_back(streams);
    state_.store(PacketSinkState::kRunning);
  }

  void OnPacket(const PacketReady& packet) noexcept override {
    calls_.push_back({CallKind::kPacket, packet.generation});
    borrowed_packets_.push_back(
        {packet.packet->data, PacketHash(packet.packet)});
    queue_.Push(packet);
  }

  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    calls_.push_back({CallKind::kReset, reset.generation});
    resets_.push_back(reset);
  }

  void OnInputEnded(const StreamEnded& end) noexcept override {
    calls_.push_back({CallKind::kEnd, end.generation});
    ends_.push_back(end);
    state_.store(PacketSinkState::kDraining);
  }

  void Stop() noexcept override {}

  PacketSinkState state() const noexcept { return state_.load(); }
  std::size_t queued() const { return queue_.size(); }
  const std::vector<ObservedCall>& calls() const { return calls_; }
  const std::vector<StreamsReady>& streams() const { return streams_; }
  const std::vector<TimelineReset>& resets() const { return resets_; }
  const std::vector<StreamEnded>& ends() const { return ends_; }
  const std::vector<BorrowedPacket>& borrowed_packets() const {
    return borrowed_packets_;
  }

  // Called only after Input::Stop has released its observer.
  std::vector<PacketReady> Drain() {
    queue_.Close();
    std::vector<PacketReady> packets;
    while (auto packet = queue_.WaitPop()) {
      packets.push_back(std::move(*packet));
    }
    const bool ended_at_eof =
        !ends_.empty() && ends_.back().reason == StreamEndReason::kEof;
    state_.store(ended_at_eof ? PacketSinkState::kEnded
                              : PacketSinkState::kStopped);
    return packets;
  }

 private:
  BlockingQueue<PacketReady> queue_;
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
  std::vector<ObservedCall> calls_;
  std::vector<StreamsReady> streams_;
  std::vector<TimelineReset> resets_;
  std::vector<StreamEnded> ends_;
  std::vector<BorrowedPacket> borrowed_packets_;
};

class FanoutObserver final : public Input::Observer {
 public:
  FanoutObserver() {
    sinks_.push_back(std::make_unique<QueuedSink>("first"));
    sinks_.push_back(std::make_unique<QueuedSink>("second"));
  }

  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    Record(CallKind::kStreams, streams.generation);
    ++stream_count_;
    for (const auto& sink : sinks_) {
      sink->OnStreamsReady(streams);
      synchronous_submission_ &= sink->streams().size() == stream_count_;
    }
  }

  void OnPacket(const PacketReady& packet) noexcept override {
    Record(CallKind::kPacket, packet.generation);
    ++packet_count_;
    for (const auto& sink : sinks_) {
      sink->OnPacket(packet);
      synchronous_submission_ &= sink->queued() == packet_count_;
    }
  }

  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    Record(CallKind::kReset, reset.generation);
    ++reset_count_;
    for (const auto& sink : sinks_) {
      sink->OnTimelineReset(reset);
      synchronous_submission_ &= sink->resets().size() == reset_count_;
    }
  }

  void OnInputEnded(const StreamEnded& end) noexcept override {
    Record(CallKind::kEnd, end.generation);
    ++end_count_;
    for (const auto& sink : sinks_) {
      sink->OnInputEnded(end);
      synchronous_submission_ &= sink->ends().size() == end_count_;
    }
  }

  void OnInputStateChanged(const InputStateChanged& state) noexcept override {
    Record(CallKind::kState, state.generation);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      states_.push_back(state);
    }
    condition_.notify_all();
  }

  bool WaitFor(InputState state) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, 5s, [&]() {
      for (const auto& observed : states_) {
        if (observed.state == state) {
          return true;
        }
      }
      return false;
    });
  }

  QueuedSink& sink(std::size_t index) { return *sinks_.at(index); }
  std::size_t event_count() const { return calls_.size(); }
  const std::vector<ObservedCall>& calls() const { return calls_; }
  const std::vector<InputStateChanged>& states() const { return states_; }
  bool synchronous_submission() const { return synchronous_submission_; }
  bool serialized_thread() const { return serialized_thread_; }
  std::thread::id delivery_thread() const { return delivery_thread_; }

 private:
  void Record(CallKind kind, std::uint64_t generation) {
    const auto thread = std::this_thread::get_id();
    if (calls_.empty()) {
      delivery_thread_ = thread;
    }
    serialized_thread_ &= delivery_thread_ == thread;
    calls_.push_back({kind, generation});
  }

  std::vector<std::unique_ptr<QueuedSink>> sinks_;
  std::vector<ObservedCall> calls_;
  std::size_t stream_count_ = 0;
  std::size_t packet_count_ = 0;
  std::size_t reset_count_ = 0;
  std::size_t end_count_ = 0;
  bool synchronous_submission_ = true;
  bool serialized_thread_ = true;
  std::thread::id delivery_thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<InputStateChanged> states_;
};

}  // namespace

TEST_CASE(
    "pipeline input preserves ordered typed calls and sink owned packet "
    "queues") {
  FanoutObserver observer;
  auto input = std::make_unique<ZlmInput>(SampleConfig());
  CHECK(input->state() == InputState::kIdle);
  input->Start(observer);
  const bool ended = observer.WaitFor(InputState::kEnded);
  const auto source_state = input->state();
  const auto consumer_state = observer.sink(0).state();
  input->Stop();
  const auto event_count = observer.event_count();
  input->Stop();
  input.reset();

  REQUIRE(ended);
  CHECK(source_state == InputState::kEnded);
  CHECK(consumer_state == PacketSinkState::kDraining);
  CHECK(observer.event_count() == event_count);
  CHECK(observer.synchronous_submission());
  CHECK(observer.serialized_thread());
  CHECK(observer.delivery_thread() != std::this_thread::get_id());
  CHECK(observer.sink(0).queued() == 115);
  CHECK(observer.sink(1).queued() == 115);
  REQUIRE(observer.sink(0).streams().size() == 1);
  REQUIRE(observer.sink(1).streams().size() == 1);
  REQUIRE(observer.sink(0).ends().size() == 1);
  REQUIRE(observer.sink(1).ends().size() == 1);
  CHECK(observer.sink(0).resets().empty());
  CHECK(observer.sink(1).resets().empty());

  const auto& streams = observer.sink(0).streams().front();
  REQUIRE(streams.streams.size() == 2);
  const auto generation = streams.generation;
  int video_index = -1;
  int audio_index = -1;
  for (const auto& stream : streams.streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_index = stream.stream_index;
    } else if (stream.codec_parameters.get()->codec_type ==
               AVMEDIA_TYPE_AUDIO) {
      audio_index = stream.stream_index;
    }
  }
  REQUIRE(video_index >= 0);
  REQUIRE(audio_index >= 0);

  const auto& calls = observer.sink(0).calls();
  const auto& other_calls = observer.sink(1).calls();
  REQUIRE(calls.size() == 117);
  REQUIRE(other_calls.size() == calls.size());
  for (std::size_t i = 0; i < calls.size(); ++i) {
    CHECK(calls[i].generation == generation);
    CHECK(calls[i].kind == other_calls[i].kind);
    CHECK(calls[i].generation == other_calls[i].generation);
    const auto expected = i == 0                  ? CallKind::kStreams
                          : i == calls.size() - 1 ? CallKind::kEnd
                                                  : CallKind::kPacket;
    CHECK(calls[i].kind == expected);
  }
  CHECK(observer.sink(0).ends().front().reason == StreamEndReason::kEof);
  CHECK(observer.sink(1).ends().front().reason == StreamEndReason::kEof);

  std::size_t state_index = 0;
  bool saw_ended_state = false;
  for (std::size_t i = 0; i < observer.calls().size(); ++i) {
    if (observer.calls()[i].kind != CallKind::kState) {
      continue;
    }
    REQUIRE(state_index < observer.states().size());
    if (observer.states()[state_index++].state == InputState::kEnded) {
      REQUIRE(i > 0);
      CHECK(observer.calls()[i - 1].kind == CallKind::kEnd);
      saw_ended_state = true;
    }
  }
  CHECK(saw_ended_state);
  CHECK(event_count == calls.size() + observer.states().size());

  const auto first = observer.sink(0).Drain();
  CHECK(observer.sink(1).queued() == 115);
  CHECK(observer.sink(1).state() == PacketSinkState::kDraining);
  const auto second = observer.sink(1).Drain();
  REQUIRE(first.size() == 115);
  REQUIRE(second.size() == first.size());
  REQUIRE(observer.sink(0).borrowed_packets().size() == first.size());
  REQUIRE(observer.sink(1).borrowed_packets().size() == first.size());
  std::size_t video_packets = 0;
  std::size_t audio_packets = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    const auto& packet = first[i];
    const auto& other = second[i];
    CHECK(packet.generation == generation);
    CHECK(other.generation == generation);
    REQUIRE(packet.packet->size > 0);
    CHECK(other.packet->data == packet.packet->data);
    CHECK(packet.packet->data == observer.sink(0).borrowed_packets()[i].data);
    CHECK(PacketHash(packet.packet) ==
          observer.sink(0).borrowed_packets()[i].hash);
    CHECK(PacketHash(other.packet) ==
          observer.sink(1).borrowed_packets()[i].hash);
    video_packets += packet.packet->stream_index == video_index;
    audio_packets += packet.packet->stream_index == audio_index;
  }
  CHECK(video_packets == 20);
  CHECK(audio_packets == 95);
  CHECK(observer.sink(0).state() == PacketSinkState::kEnded);
  CHECK(observer.sink(1).state() == PacketSinkState::kEnded);
}

TEST_CASE("pipeline input explicit stop differs from EOF and ends once") {
  FanoutObserver observer;
  ZlmInput input(SampleConfig());
  input.Start(observer);
  const bool ready = observer.WaitFor(InputState::kReady);
  input.Stop();
  const auto count = observer.event_count();
  input.Stop();

  REQUIRE(ready);
  CHECK(input.state() == InputState::kStopped);
  CHECK(observer.event_count() == count);
  CHECK(observer.sink(0).state() == PacketSinkState::kDraining);
  REQUIRE(observer.sink(0).ends().size() == 1);
  CHECK(observer.sink(0).ends().front().reason == StreamEndReason::kStopped);
  bool saw_stopped_state = false;
  std::size_t state_index = 0;
  for (std::size_t i = 0; i < observer.calls().size(); ++i) {
    if (observer.calls()[i].kind != CallKind::kState) {
      continue;
    }
    REQUIRE(state_index < observer.states().size());
    const auto state = observer.states()[state_index++].state;
    CHECK(state != InputState::kEnded);
    if (state == InputState::kStopped) {
      REQUIRE(i > 0);
      CHECK(observer.calls()[i - 1].kind == CallKind::kEnd);
      saw_stopped_state = true;
    }
  }
  CHECK(saw_stopped_state);
  observer.sink(0).Drain();
  CHECK(observer.sink(0).state() == PacketSinkState::kStopped);
}

TEST_CASE(
    "pipeline input reports source failure without ending unopened streams") {
  FanoutObserver observer;
  auto config = SampleConfig();
  config.url += ".missing";
  ZlmInput input(std::move(config));
  input.Start(observer);
  const bool failed = observer.WaitFor(InputState::kFailed);
  const auto source_state = input.state();
  input.Stop();

  REQUIRE(failed);
  CHECK(source_state == InputState::kFailed);
  CHECK(observer.sink(0).calls().empty());
  CHECK(observer.sink(1).calls().empty());
  CHECK(observer.sink(0).queued() == 0);
  CHECK(observer.sink(0).state() == PacketSinkState::kIdle);
  CHECK(observer.event_count() == observer.states().size());
  bool saw_failure = false;
  for (const auto& state : observer.states()) {
    if (state.state == InputState::kFailed) {
      CHECK_FALSE(state.error.empty());
      CHECK_FALSE(state.will_retry);
      saw_failure = true;
    }
  }
  CHECK(saw_failure);
}

TEST_CASE(
    "pipeline input validates configuration and enforces one shot start") {
  FanoutObserver observer;
  SECTION("empty URL fails synchronously") {
    auto config = SampleConfig();
    config.url.clear();
    ZlmInput input(std::move(config));
    CHECK_THROWS_AS(input.Start(observer), std::invalid_argument);
    input.Stop();
    CHECK(observer.event_count() == 0);
  }
  SECTION("duplicate start cannot replace the observer") {
    FanoutObserver replacement;
    ZlmInput input(SampleConfig());
    input.Start(observer);
    CHECK_THROWS_AS(input.Start(replacement), std::logic_error);
    input.Stop();
    CHECK_THROWS_AS(input.Start(replacement), std::logic_error);
    CHECK(replacement.event_count() == 0);
  }
  SECTION("stop before start prevents a later start") {
    ZlmInput input(SampleConfig());
    input.Stop();
    CHECK_THROWS_AS(input.Start(observer), std::logic_error);
    CHECK(observer.event_count() == 0);
  }
}

TEST_CASE("pipeline input stop waits for its borrowed observer to return") {
  class BlockingObserver final : public Input::Observer {
   public:
    explicit BlockingObserver(std::shared_future<void> release)
        : release_(std::move(release)) {}

    void OnStreamsReady(const StreamsReady&) noexcept override {
      ++event_count_;
    }

    void OnPacket(const PacketReady&) noexcept override {
      if (!blocked_) {
        blocked_ = true;
        entered_.set_value();
        release_.wait();
      }
      ++event_count_;
    }

    void OnTimelineReset(const TimelineReset&) noexcept override {
      ++event_count_;
    }
    void OnInputEnded(const StreamEnded&) noexcept override { ++event_count_; }
    void OnInputStateChanged(const InputStateChanged&) noexcept override {
      ++event_count_;
    }

    std::promise<void> entered_;
    std::atomic<std::size_t> event_count_{0};

   private:
    std::shared_future<void> release_;
    bool blocked_ = false;
  };

  std::promise<void> release;
  BlockingObserver observer(release.get_future().share());
  auto entered = observer.entered_.get_future();
  auto input = std::make_unique<ZlmInput>(SampleConfig());
  input->Start(observer);
  const auto entered_result = entered.wait_for(5s);
  std::promise<void> stop_started;
  auto started = stop_started.get_future();
  auto stopped = std::async(std::launch::async, [&]() {
    stop_started.set_value();
    input->Stop();
  });
  const auto started_result = started.wait_for(5s);
  const auto blocked_result = stopped.wait_for(100ms);
  // Always unblock the source before asserting, including on timeout paths.
  release.set_value();
  const auto stopped_result = stopped.wait_for(5s);

  REQUIRE(entered_result == std::future_status::ready);
  REQUIRE(started_result == std::future_status::ready);
  CHECK(blocked_result == std::future_status::timeout);
  REQUIRE(stopped_result == std::future_status::ready);
  stopped.get();
  const auto event_count = observer.event_count_.load();
  input.reset();
  CHECK(observer.event_count_.load() == event_count);
}

TEST_CASE("pipeline inputs deliver packets on distinct exclusive Pollers") {
  struct Observation {
    toolkit::EventPoller::Ptr current;
    toolkit::EventPoller::Ptr shared;
  };
  class PollerObserver final : public Input::Observer {
   public:
    void OnStreamsReady(const StreamsReady&) noexcept override {}
    void OnPacket(const PacketReady&) noexcept override {
      if (!observed_) {
        observed_ = true;
        result.set_value({toolkit::EventPoller::getCurrentPoller(),
                          toolkit::EventPollerPool::Instance().getPoller()});
      }
    }
    void OnTimelineReset(const TimelineReset&) noexcept override {}
    void OnInputEnded(const StreamEnded&) noexcept override {}
    void OnInputStateChanged(const InputStateChanged&) noexcept override {}

    std::promise<Observation> result;

   private:
    bool observed_ = false;
  };

  // Inputs are destroyed before their borrowed observers, including if Start
  // throws or an assertion fails. No callback blocks the Poller in this test.
  PollerObserver first_observer;
  PollerObserver second_observer;
  auto first_result = first_observer.result.get_future();
  auto second_result = second_observer.result.get_future();
  ZlmInput first(SampleConfig());
  ZlmInput second(SampleConfig());
  first.Start(first_observer);
  second.Start(second_observer);
  const auto first_status = first_result.wait_for(5s);
  const auto second_status = second_result.wait_for(5s);
  first.Stop();
  second.Stop();

  REQUIRE(first_status == std::future_status::ready);
  REQUIRE(second_status == std::future_status::ready);
  const auto first_poller = first_result.get();
  const auto second_poller = second_result.get();
  REQUIRE(first_poller.current);
  REQUIRE(second_poller.current);
  CHECK(first_poller.current != second_poller.current);
  CHECK(first_poller.shared != first_poller.current);
  CHECK(second_poller.shared != second_poller.current);
  toolkit::EventPollerPool::Instance().for_each(
      [&](const toolkit::TaskExecutor::Ptr& shared) {
        CHECK(shared != first_poller.current);
        CHECK(shared != second_poller.current);
      });
}

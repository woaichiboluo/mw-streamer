#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/streamer/input/player_proxy.h"
#include "mw/streamer/sink/packet_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::Packet;
using mw::streamer::StreamInfo;
using mw::streamer::ControlResult;
using mw::streamer::PlayerProxy;
using mw::streamer::PlayerState;
using mw::streamer::PacketSink;

std::string SamplePath() {
  return std::string(MW_INPUT_PLAYER_PROXY_TEST_DATA_DIR) + "/h264_aac.mp4";
}

enum class EventKind { kStreams, kPacket, kEnd };

std::uint64_t PacketHash(const Packet& packet) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (int i = 0; i < packet->size; ++i) {
    hash = (hash ^ packet->data[i]) * 1099511628211ULL;
  }
  return hash;
}

struct Event {
  EventKind kind;
  int consumer;
  std::uint64_t generation;
  std::uint64_t hash = 0;
  const std::uint8_t* data = nullptr;
};

struct Observation {
  std::vector<Event> events;
  bool on_owner_thread = true;
  std::size_t destroyed_sinks = 0;
};

class RecordingSink final : public PacketSink {
 public:
  RecordingSink(int consumer, std::shared_ptr<toolkit::EventPoller> poller,
                std::shared_ptr<Observation> observation)
      : consumer_(consumer),
        poller_(std::move(poller)),
        observation_(std::move(observation)) {}

  ~RecordingSink() override { ++observation_->destroyed_sinks; }

  void SetStreams(std::uint64_t generation,
                  const std::vector<StreamInfo>& streams) noexcept override {
    Observe(EventKind::kStreams, generation);
    stream_count_ = streams.size();
  }

  void Write(std::uint64_t generation, const Packet& packet) noexcept override {
    Observe(EventKind::kPacket, generation, PacketHash(packet), packet->data);
    packets_.push_back(packet.Ref());
  }

  void EndInput(std::uint64_t generation) noexcept override {
    Observe(EventKind::kEnd, generation);
  }

  std::size_t stream_count() const { return stream_count_; }
  const std::vector<Packet>& packets() const { return packets_; }

 private:
  void Observe(EventKind kind, std::uint64_t generation, std::uint64_t hash = 0,
               const std::uint8_t* data = nullptr) {
    observation_->on_owner_thread &= poller_->isCurrentThread();
    observation_->events.push_back({kind, consumer_, generation, hash, data});
  }

  int consumer_;
  std::shared_ptr<toolkit::EventPoller> poller_;
  std::shared_ptr<Observation> observation_;
  std::size_t stream_count_ = 0;
  std::vector<Packet> packets_;
};

void StopAndWait(const PlayerProxy::Ptr& proxy) {
  auto stopped = std::make_shared<std::promise<void>>();
  auto completion = stopped->get_future();
  proxy->Stop([stopped]() { stopped->set_value(); });
  REQUIRE(completion.wait_for(5s) == std::future_status::ready);
}

void FlushPoller(const std::shared_ptr<toolkit::EventPoller>& poller) {
  auto flushed = std::make_shared<std::promise<void>>();
  auto completion = flushed->get_future();
  poller->async([flushed]() { flushed->set_value(); }, false);
  REQUIRE(completion.wait_for(5s) == std::future_status::ready);
}

}  // namespace

TEST_CASE("input synchronously fans packets out in sink registration order") {
  auto observation = std::make_shared<Observation>();
  auto proxy = std::make_shared<PlayerProxy>();
  auto first = std::make_unique<RecordingSink>(1, proxy->poller(), observation);
  auto second =
      std::make_unique<RecordingSink>(2, proxy->poller(), observation);
  auto* first_sink = first.get();
  auto* second_sink = second.get();
  proxy->AddPacketSink(std::move(first));
  proxy->AddPacketSink(std::move(second));
  auto ended = std::make_shared<std::promise<void>>();
  auto completion = ended->get_future();
  proxy->SetOnState([ended](std::uint64_t, PlayerState state,
                            const toolkit::SockException&, bool) {
    if (state == PlayerState::kEnded) {
      ended->set_value();
    }
  });
  proxy->Start(SamplePath());
  const auto result = completion.wait_for(5s);
  StopAndWait(proxy);
  StopAndWait(proxy);

  REQUIRE(result == std::future_status::ready);
  CHECK(observation->on_owner_thread);
  CHECK(first_sink->stream_count() == 2);
  CHECK(second_sink->stream_count() == 2);
  REQUIRE(first_sink->packets().size() == 115);
  REQUIRE(second_sink->packets().size() == 115);
  REQUIRE(observation->events.size() == (115 + 2) * 2);
  for (std::size_t group = 0; group < 117; ++group) {
    const auto expected_kind = group == 0     ? EventKind::kStreams
                               : group == 116 ? EventKind::kEnd
                                              : EventKind::kPacket;
    for (std::size_t offset = 0; offset < 2; ++offset) {
      const auto& event = observation->events[group * 2 + offset];
      CHECK(event.kind == expected_kind);
      CHECK(event.consumer == static_cast<int>(offset + 1));
      CHECK(event.generation == 1);
      if (expected_kind == EventKind::kPacket) {
        const auto& retained = first_sink->packets()[group - 1];
        CHECK(event.hash == PacketHash(retained));
        CHECK(event.data == retained->data);
        CHECK(event.hash == PacketHash(second_sink->packets()[group - 1]));
      }
    }
  }
}

TEST_CASE(
    "input seek initializes sink generation before delivering new packets") {
  auto observation = std::make_shared<Observation>();
  auto proxy = std::make_shared<PlayerProxy>();
  proxy->AddPacketSink(
      std::make_unique<RecordingSink>(1, proxy->poller(), observation));
  auto ready = std::make_shared<std::promise<void>>();
  auto ready_completion = ready->get_future();
  auto ended = std::make_shared<std::promise<void>>();
  auto end_completion = ended->get_future();
  proxy->SetOnState([ready, ended](std::uint64_t, PlayerState state,
                                   const toolkit::SockException&, bool) {
    if (state == PlayerState::kReady) {
      ready->set_value();
    } else if (state == PlayerState::kEnded) {
      ended->set_value();
    }
  });
  proxy->Start(SamplePath());
  REQUIRE(ready_completion.wait_for(5s) == std::future_status::ready);
  auto paused = std::make_shared<std::promise<ControlResult>>();
  auto pause_completion = paused->get_future();
  proxy->Pause(true, [paused](ControlResult result, std::uint64_t) {
    paused->set_value(result);
  });
  REQUIRE(pause_completion.wait_for(5s) == std::future_status::ready);
  REQUIRE(pause_completion.get() == ControlResult::kAccepted);
  auto seeked = std::make_shared<std::promise<ControlResult>>();
  auto seek_completion = seeked->get_future();
  proxy->SeekTo(1000ms, [seeked](ControlResult result, std::uint64_t) {
    seeked->set_value(result);
  });
  REQUIRE(seek_completion.wait_for(5s) == std::future_status::ready);
  REQUIRE(seek_completion.get() == ControlResult::kAccepted);
  proxy->SetPlaybackRate(20.0f);
  const auto result = end_completion.wait_for(5s);
  StopAndWait(proxy);

  REQUIRE(result == std::future_status::ready);
  CHECK(observation->on_owner_thread);
  std::uint64_t generation = 0;
  std::size_t streams = 0;
  std::size_t ends = 0;
  std::size_t new_packets = 0;
  for (const auto& event : observation->events) {
    if (event.kind == EventKind::kStreams) {
      CHECK(event.generation == generation + 1);
      generation = event.generation;
      ++streams;
    } else {
      CHECK(event.generation == generation);
      if (event.kind == EventKind::kEnd) {
        CHECK(event.generation == 2);
        ++ends;
      } else if (event.generation == 2) {
        ++new_packets;
      }
    }
  }
  CHECK(streams == 2);
  CHECK(ends == 1);
  CHECK(new_packets > 0);
}

TEST_CASE("explicit input stop ends each opened sink generation only once") {
  auto observation = std::make_shared<Observation>();
  auto proxy = std::make_shared<PlayerProxy>();
  proxy->AddPacketSink(
      std::make_unique<RecordingSink>(1, proxy->poller(), observation));
  auto ready = std::make_shared<std::promise<void>>();
  auto completion = ready->get_future();
  proxy->SetOnState([ready](std::uint64_t, PlayerState state,
                            const toolkit::SockException&, bool) {
    if (state == PlayerState::kReady) {
      ready->set_value();
    }
  });
  proxy->Start(SamplePath());
  const auto result = completion.wait_for(5s);
  StopAndWait(proxy);
  StopAndWait(proxy);
  auto restarted = std::make_shared<std::promise<void>>();
  auto restart_completion = restarted->get_future();
  proxy->SetOnState([restarted](std::uint64_t, PlayerState state,
                                const toolkit::SockException&, bool) {
    if (state == PlayerState::kReady) {
      restarted->set_value();
    }
  });
  proxy->Start(SamplePath());
  const auto restart_result = restart_completion.wait_for(5s);
  StopAndWait(proxy);
  StopAndWait(proxy);
  const auto poller = proxy->poller();
  proxy.reset();
  FlushPoller(poller);

  REQUIRE(result == std::future_status::ready);
  REQUIRE(restart_result == std::future_status::ready);
  REQUIRE(observation->events.size() >= 4);
  CHECK(observation->events.front().kind == EventKind::kStreams);
  CHECK(observation->events.back().kind == EventKind::kEnd);
  std::size_t ends = 0;
  std::size_t streams = 0;
  for (const auto& event : observation->events) {
    if (event.kind == EventKind::kStreams) {
      CHECK(streams == ends);
      ++streams;
    } else if (event.kind == EventKind::kEnd) {
      ++ends;
      CHECK(streams == ends);
    }
    CHECK(event.generation == streams);
  }
  CHECK(streams == 2);
  CHECK(ends == 2);
  CHECK(observation->on_owner_thread);
}

TEST_CASE("destroying active input ends the sink and releases its ownership") {
  auto observation = std::make_shared<Observation>();
  auto proxy = std::make_shared<PlayerProxy>();
  const auto poller = proxy->poller();
  auto sink = std::make_unique<RecordingSink>(1, poller, observation);
  proxy->AddPacketSink(std::move(sink));
  CHECK(observation->destroyed_sinks == 0);
  auto ready = std::make_shared<std::promise<void>>();
  auto completion = ready->get_future();
  proxy->SetOnState([ready](std::uint64_t, PlayerState state,
                            const toolkit::SockException&, bool) {
    if (state == PlayerState::kReady) {
      ready->set_value();
    }
  });
  proxy->Start(SamplePath());
  const auto result = completion.wait_for(5s);
  proxy.reset();
  FlushPoller(poller);

  REQUIRE(result == std::future_status::ready);
  CHECK(observation->destroyed_sinks == 1);
  REQUIRE(observation->events.size() >= 2);
  CHECK(observation->events.front().kind == EventKind::kStreams);
  CHECK(observation->events.back().kind == EventKind::kEnd);
  std::size_t ends = 0;
  for (const auto& event : observation->events) {
    ends += event.kind == EventKind::kEnd;
  }
  CHECK(ends == 1);
  CHECK(observation->on_owner_thread);
}

TEST_CASE(
    "input rejects invalid sink registration and never ends unopened sinks") {
  auto observation = std::make_shared<Observation>();
  auto proxy = std::make_shared<PlayerProxy>();
  auto sink = std::make_unique<RecordingSink>(1, proxy->poller(), observation);
  CHECK_THROWS_AS(proxy->AddPacketSink(nullptr), std::invalid_argument);
  proxy->AddPacketSink(std::move(sink));
  auto failed = std::make_shared<std::promise<void>>();
  auto completion = failed->get_future();
  proxy->SetOnState([failed](std::uint64_t, PlayerState state,
                             const toolkit::SockException&, bool) {
    if (state == PlayerState::kFailed) {
      failed->set_value();
    }
  });
  auto rejected_observation = std::make_shared<Observation>();
  SECTION("registration after an off-poller Start is rejected") {
    proxy->Start(SamplePath() + ".missing");
  }
  SECTION("registration in the same owner-poller task as Start is rejected") {
    auto rejected = std::make_shared<std::promise<bool>>();
    auto rejection_completion = rejected->get_future();
    proxy->poller()->async(
        [proxy, rejected, rejected_observation]() {
          proxy->Start(SamplePath() + ".missing");
          bool caught = false;
          try {
            proxy->AddPacketSink(std::make_unique<RecordingSink>(
                2, proxy->poller(), rejected_observation));
          } catch (const std::logic_error&) {
            caught = true;
          }
          rejected->set_value(caught);
        },
        false);
    REQUIRE(rejection_completion.wait_for(5s) == std::future_status::ready);
    CHECK(rejection_completion.get());
  }
  const auto result = completion.wait_for(5s);
  CHECK_THROWS_AS(proxy->AddPacketSink(std::make_unique<RecordingSink>(
                      2, proxy->poller(), rejected_observation)),
                  std::logic_error);
  StopAndWait(proxy);
  CHECK_THROWS_AS(proxy->AddPacketSink(std::make_unique<RecordingSink>(
                      2, proxy->poller(), rejected_observation)),
                  std::logic_error);
  CHECK(observation->destroyed_sinks == 0);
  const auto poller = proxy->poller();
  proxy.reset();
  FlushPoller(poller);

  REQUIRE(result == std::future_status::ready);
  CHECK(observation->destroyed_sinks == 1);
  CHECK(observation->events.empty());
}

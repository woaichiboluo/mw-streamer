#include "mw/streamer/cache/packet_queue.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mw::streamer::PacketQueue;
using mw::streamer::PacketReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;
using mw::streamer::TimelineResetReason;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using mw::streamer::CodecParameters;
using mw::streamer::Packet;
using mw::streamer::StreamInfo;
using Clock = std::chrono::steady_clock;

StreamInfo Stream(int index, AVMediaType type,
                          AVRational time_base = {1, 1000}) {
  CodecParameters parameters;
  parameters.get()->codec_type = type;
  parameters.get()->codec_id =
      type == AVMEDIA_TYPE_AUDIO ? AV_CODEC_ID_AAC : AV_CODEC_ID_H264;
  return {index, std::move(parameters), time_base};
}

Packet MakePacket(int index, std::int64_t dts) {
  Packet packet;
  if (av_new_packet(packet.get(), 4) < 0) {
    throw std::runtime_error("测试Packet分配失败");
  }
  packet->stream_index = index;
  packet->dts = dts;
  packet->pts = dts;
  packet->data[0] = 42;
  return packet;
}

struct Event {
  std::string kind;
  std::uint64_t generation;
  std::thread::id thread;
};

class Recorder final : public Sink {
 public:
  Recorder() : Sink("recorder", SinkMediaType::kPacket) {}
  void OnStreamsReady(const StreamsReady& ready) noexcept override {
    state_.store(PacketSinkState::kRunning);
    Record("ready", ready.generation);
  }

  void OnPacket(const PacketReady& packet) noexcept override {
    {
      std::unique_lock<std::mutex> lock(mutex);
      RecordLocked("packet", packet.generation);
      packets.push_back(packet);
      packet_times.push_back(Clock::now());
      changed.notify_all();
      if (block_packet) {
        changed.wait(lock, [&] { return release_packet; });
      }
    }
    if (abort_on_packet) {
      abort_on_packet->Abort();
      std::lock_guard<std::mutex> lock(mutex);
      callback_aborted = true;
      changed.notify_all();
    }
  }

  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    Record("reset", reset.generation);
  }

  void OnInputEnded(const StreamEnded& end) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);
    ends.push_back(end);
    // Receiving input EOF does not complete a consumer's own pending work.
    state_.store(PacketSinkState::kDraining);
    RecordLocked("end", end.generation);
    changed.notify_all();
  }

  void Stop() noexcept override {
    std::lock_guard<std::mutex> lock(mutex);
    if (state_.exchange(PacketSinkState::kStopped) !=
        PacketSinkState::kStopped) {
      ++stop_calls;
    }
  }

  PacketSinkState state() const noexcept { return state_.load(); }

  template <typename Predicate>
  bool Wait(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, predicate);
  }

  bool WaitPackets(std::size_t count, std::chrono::milliseconds timeout = 5s) {
    return Wait([&] { return packets.size() >= count; }, timeout);
  }

  bool WaitEnded(std::size_t count = 1) {
    return Wait([&] { return ends.size() >= count; });
  }

  void ReleasePacket() {
    std::lock_guard<std::mutex> lock(mutex);
    release_packet = true;
    changed.notify_all();
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Event> events;
  std::vector<PacketReady> packets;
  std::vector<Clock::time_point> packet_times;
  std::vector<StreamEnded> ends;
  int stop_calls = 0;
  bool block_packet = false;
  bool release_packet = false;
  bool callback_aborted = false;
  PacketQueue* abort_on_packet = nullptr;

 private:
  void Record(const char* kind, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex);
    RecordLocked(kind, generation);
    changed.notify_all();
  }

  void RecordLocked(const char* kind, std::uint64_t generation) {
    events.push_back({kind, generation, std::this_thread::get_id()});
  }

  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
};

bool WaitState(const PacketQueue& queue, PacketSinkState expected) {
  const auto deadline = Clock::now() + 5s;
  while (queue.state() != expected && Clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return queue.state() == expected;
}

}  // namespace

TEST_CASE(
    "Pipeline PacketQueue asynchronously preserves order and packet buffers") {
  Recorder recorder;
  auto first = MakePacket(0, 10);
  const auto* buffer = first->buf->buffer;
  {
    PacketQueue queue(0ms, recorder);
    CHECK(queue.state() == PacketSinkState::kIdle);
    CHECK(queue.generation() == 0);
    queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
    queue.OnPacket({1, first});
    first.Unref();
    queue.OnPacket({1, MakePacket(0, 20)});
    queue.OnInputEnded({1, StreamEndReason::kEof});
    const bool completed = recorder.WaitEnded();
    const bool dequeued = WaitState(queue, PacketSinkState::kEnded);
    queue.Stop();
    REQUIRE(completed);
    REQUIRE(dequeued);
    REQUIRE(queue.error().empty());
    CHECK(queue.generation() == 1);
  }
  CHECK(recorder.stop_calls == 0);
  CHECK(recorder.state() == PacketSinkState::kDraining);
  REQUIRE(recorder.packets.size() == 2);
  CHECK(recorder.packets[0].packet->buf->buffer == buffer);
  CHECK(recorder.packets[0].packet->data[0] == 42);
  CHECK(recorder.packets[0].packet->dts == 10);
  CHECK(recorder.packets[1].packet->dts == 20);
  REQUIRE(recorder.events.size() == 4);
  std::vector<std::string> kinds;
  for (const auto& event : recorder.events) {
    kinds.push_back(event.kind);
    CHECK(event.thread == recorder.events.front().thread);
    CHECK(event.thread != std::this_thread::get_id());
  }
  CHECK(kinds == std::vector<std::string>{"ready", "packet", "packet", "end"});
}

TEST_CASE(
    "Pipeline PacketQueue shares a clock across independently buffered "
    "tracks") {
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  queue.OnStreamsReady({1,
                        {Stream(0, AVMEDIA_TYPE_VIDEO),
                         Stream(1, AVMEDIA_TYPE_AUDIO, {1, 48000})}});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({1, MakePacket(0, 1000)});
  queue.OnPacket({1, MakePacket(1, 0)});
  const bool early_packet = recorder.WaitPackets(1, 100ms);
  queue.OnPacket({1, MakePacket(1, 48000)});
  const bool clock_advanced = recorder.WaitPackets(3);
  queue.OnInputEnded({1, StreamEndReason::kEof});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  CHECK_FALSE(early_packet);
  REQUIRE(clock_advanced);
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  REQUIRE(recorder.packets.size() == 4);
  CHECK(recorder.packets[0].packet->stream_index == 0);
  CHECK(recorder.packets[1].packet->stream_index == 1);
  CHECK(recorder.packets[2].packet->dts == 1000);
  CHECK(recorder.packets[3].packet->dts == 48000);
  CHECK(recorder.packet_times[2] - recorder.packet_times[0] >= 850ms);
}

TEST_CASE(
    "Pipeline PacketQueue preserves its generation clock after temporary "
    "starvation") {
  bool audio = false;
  SECTION("single track becomes empty") {}
  SECTION("video becomes empty while audio remains buffered") { audio = true; }
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  auto streams = std::vector{Stream(0, AVMEDIA_TYPE_VIDEO)};
  if (audio) streams.push_back(Stream(1, AVMEDIA_TYPE_AUDIO));
  queue.OnStreamsReady({1, streams});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({1, MakePacket(0, 1000)});
  if (audio) {
    queue.OnPacket({1, MakePacket(1, 0)});
    queue.OnPacket({1, MakePacket(1, 2000)});
  }
  const std::size_t initial_count = audio ? 3 : 2;
  REQUIRE(recorder.WaitPackets(initial_count));
  // Let the scheduler observe starvation. Resume with a late packet but less
  // than a new cache window; the original deadline must still apply.
  std::this_thread::sleep_for(600ms);
  queue.OnPacket({1, MakePacket(0, 1100)});
  const bool resumed = recorder.WaitPackets(initial_count + 1, 250ms);
  queue.OnPacket({1, MakePacket(0, 2000)});
  queue.OnInputEnded({1, StreamEndReason::kEof});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  CHECK(resumed);
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  REQUIRE(recorder.packets.size() == (audio ? 6 : 4));
  CHECK(recorder.packets[initial_count].packet->dts == 1100);
  CHECK(recorder.packets[initial_count + 1].packet->dts == 2000);
  const auto final_delay =
      recorder.packet_times[initial_count + 1] - recorder.packet_times.front();
  CHECK(final_delay >= 1850ms);
  CHECK(final_delay < 2400ms);
  if (audio) {
    // Waiting for the missing video track must not reorder the retained audio.
    CHECK(recorder.packets.back().packet->stream_index == 1);
  }
}

TEST_CASE(
    "Pipeline PacketQueue drains short input and unequal track tails once") {
  bool audio_only = false;
  SECTION("single audio track") { audio_only = true; }
  SECTION("audio outlasts video with a different time base") {}
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  std::vector<StreamInfo> streams{
      Stream(1, AVMEDIA_TYPE_AUDIO, {1, 48000})};
  if (!audio_only) {
    streams.push_back(Stream(0, AVMEDIA_TYPE_VIDEO));
  }
  queue.OnStreamsReady({1, streams});
  if (!audio_only) {
    queue.OnPacket({1, MakePacket(0, 0)});
    queue.OnPacket({1, MakePacket(0, 20)});
  }
  for (const auto dts : {0, 480, 960, 1440}) {
    queue.OnPacket({1, MakePacket(1, dts)});
  }
  queue.OnInputEnded({1, StreamEndReason::kEof});
  queue.OnInputEnded({1, StreamEndReason::kEof});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  REQUIRE(recorder.ends.size() == 1);
  CHECK(recorder.ends[0].reason == StreamEndReason::kEof);
  REQUIRE(recorder.packets.size() == (audio_only ? 4 : 6));
  std::int64_t previous_us = -1;
  for (const auto& ready : recorder.packets) {
    const auto& packet = ready.packet;
    const auto dts_us = packet->stream_index == 0
                            ? packet->dts * 1000
                            : packet->dts * 1000000 / 48000;
    CHECK(dts_us >= previous_us);
    previous_us = dts_us;
  }
  CHECK(previous_us == 30000);
  CHECK(recorder.events.back().kind == "end");
}

TEST_CASE(
    "Pipeline PacketQueue reset interrupts its clock wait and drops old "
    "packets") {
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  const auto streams = std::vector{Stream(0, AVMEDIA_TYPE_VIDEO)};
  queue.OnStreamsReady({1, streams});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({1, MakePacket(0, 60000)});
  const bool first_played = recorder.WaitPackets(1);
  const auto reset_time = Clock::now();
  queue.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  queue.OnStreamsReady({2, streams});
  queue.OnPacket({2, MakePacket(0, 0)});
  queue.OnInputEnded({2, StreamEndReason::kEof});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  REQUIRE(first_played);
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  REQUIRE(recorder.packets.size() == 2);
  CHECK(recorder.packets[0].generation == 1);
  CHECK(recorder.packets[1].generation == 2);
  CHECK(recorder.packet_times[1] - reset_time < 2s);
  bool reset_seen = false;
  bool ready_seen = false;
  for (const auto& event : recorder.events) {
    if (event.generation != 2) {
      continue;
    }
    if (event.kind == "reset") {
      reset_seen = true;
    } else if (event.kind == "ready") {
      CHECK(reset_seen);
      ready_seen = true;
    } else if (event.kind == "packet") {
      CHECK(ready_seen);
    }
  }
}

TEST_CASE(
    "Pipeline PacketQueue Stop waits for callbacks without blocking "
    "submission") {
  Recorder recorder;
  recorder.block_packet = true;
  PacketQueue queue(0ms, recorder);
  queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnPacket({1, MakePacket(0, 0)});
  const bool entered = recorder.WaitPackets(1);
  auto submitted = std::async(std::launch::async, [&] {
    queue.OnPacket({1, MakePacket(0, 1)});
    queue.OnInputEnded({1, StreamEndReason::kEof});
  });
  const auto submission = submitted.wait_for(100ms);
  auto stopped = std::async(std::launch::async, [&] { queue.Stop(); });
  const auto while_blocked = stopped.wait_for(100ms);
  recorder.ReleasePacket();
  submitted.get();
  stopped.get();
  queue.Stop();
  queue.OnPacket({1, MakePacket(0, 2)});
  REQUIRE(entered);
  CHECK(submission == std::future_status::ready);
  CHECK(while_blocked == std::future_status::timeout);
  CHECK(recorder.packets.size() == 1);
  CHECK(queue.error().empty());
}

TEST_CASE("Pipeline PacketQueue permits Abort from its own packet callback") {
  Recorder recorder;
  PacketQueue queue(0ms, recorder);
  recorder.abort_on_packet = &queue;
  queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({1, MakePacket(0, 1)});
  const bool aborted = recorder.Wait([&] { return recorder.callback_aborted; });
  queue.OnPacket({1, MakePacket(0, 2)});
  const bool extra_packet =
      recorder.Wait([&] { return recorder.packets.size() > 1; }, 50ms);
  queue.Stop();
  REQUIRE(aborted);
  CHECK_FALSE(extra_packet);
  CHECK(recorder.packets.size() == 1);
  CHECK(queue.error().empty());
}

TEST_CASE("Pipeline PacketQueue rejects invalid cache durations") {
  Recorder recorder;
  for (const auto duration :
       {-1ms, 1ms, 999ms, 30001ms, std::chrono::milliseconds::max()}) {
    CHECK_THROWS_AS(PacketQueue(duration, recorder), std::invalid_argument);
  }
}

TEST_CASE(
    "Pipeline PacketQueue distinguishes interrupted and discarded tails") {
  auto reason = StreamEndReason::kInterrupted;
  SECTION("interruption drains the short cached tail") {}
  SECTION("stopped input discards the short cached tail") {
    reason = StreamEndReason::kStopped;
  }
  SECTION("failed input discards the short cached tail") {
    reason = StreamEndReason::kFailed;
  }
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnInputEnded({1, reason});
  queue.OnInputEnded({1, reason});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  CHECK(recorder.packets.size() ==
        (reason == StreamEndReason::kInterrupted ? 1 : 0));
  REQUIRE(recorder.ends.size() == 1);
  CHECK(recorder.ends[0].reason == reason);
  CHECK(recorder.events.back().kind == "end");
}

TEST_CASE(
    "Pipeline PacketQueue filters invalid packets and stale generations") {
  Recorder recorder;
  PacketQueue queue(0ms, recorder);
  queue.OnStreamsReady({2, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({2, MakePacket(0, AV_NOPTS_VALUE)});
  queue.OnPacket({2, MakePacket(-1, 0)});
  queue.OnPacket({2, MakePacket(3, 0)});
  queue.OnPacket({2, MakePacket(0, 10)});
  queue.OnPacket({2, MakePacket(0, 9)});
  queue.OnPacket({2, MakePacket(0, 11)});
  queue.OnInputEnded({1, StreamEndReason::kEof});
  queue.OnInputEnded({2, StreamEndReason::kEof});
  const bool completed = recorder.WaitEnded();
  queue.Stop();
  REQUIRE(completed);
  REQUIRE(queue.error().empty());
  REQUIRE(recorder.packets.size() == 2);
  CHECK(recorder.packets[0].packet->dts == 10);
  CHECK(recorder.packets[1].packet->dts == 11);
  REQUIRE(recorder.ends.size() == 1);
  CHECK(recorder.ends[0].generation == 2);
}

TEST_CASE(
    "Pipeline PacketQueue reports configuration failures through its state") {
  Recorder recorder;
  PacketQueue queue(0ms, recorder);
  bool previously_ready = false;
  SECTION("empty initial stream configuration does not invent an end") {
    queue.OnStreamsReady({1, {}});
  }
  SECTION("invalid initial time base does not invent an end") {
    queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO, {1, 0})}});
  }
  SECTION("replacement without reset fails the existing generation") {
    previously_ready = true;
    queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
    queue.OnStreamsReady({2, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  }
  const bool failed = WaitState(queue, PacketSinkState::kFailed);
  const bool ended = !previously_ready || recorder.WaitEnded();
  const auto error = queue.error();
  queue.Abort();
  queue.Stop();
  REQUIRE(failed);
  REQUIRE(ended);
  REQUIRE_FALSE(error.empty());
  CHECK(queue.state() == PacketSinkState::kFailed);
  CHECK(queue.error() == error);
  CHECK(recorder.stop_calls == 0);
  if (previously_ready) {
    REQUIRE(recorder.ends.size() == 1);
    CHECK(recorder.ends[0].generation == 1);
    CHECK(recorder.ends[0].reason == StreamEndReason::kFailed);
  } else {
    CHECK(recorder.events.empty());
    CHECK(recorder.ends.empty());
  }
}

TEST_CASE(
    "Pipeline PacketQueue drains its cache before ending its borrowed sink") {
  Recorder recorder;
  PacketQueue queue(1s, recorder);
  queue.OnStreamsReady({1, {Stream(0, AVMEDIA_TYPE_VIDEO)}});
  queue.OnPacket({1, MakePacket(0, 0)});
  queue.OnPacket({1, MakePacket(0, 60000)});
  const bool first_played = recorder.WaitPackets(1);
  queue.OnInputEnded({1, StreamEndReason::kEof});
  const bool draining = WaitState(queue, PacketSinkState::kDraining);
  bool end_withheld = false;
  {
    std::lock_guard<std::mutex> lock(recorder.mutex);
    end_withheld = recorder.ends.empty();
  }
  const auto stop_time = Clock::now();
  queue.Stop();
  const auto stop_duration = Clock::now() - stop_time;
  REQUIRE(first_played);
  REQUIRE(draining);
  CHECK(end_withheld);
  CHECK(queue.generation() == 1);
  CHECK(stop_duration < 2s);
  CHECK(queue.error().empty());
  CHECK(recorder.stop_calls == 0);
  CHECK(recorder.state() == PacketSinkState::kRunning);
  recorder.Stop();
  recorder.Stop();
  CHECK(recorder.stop_calls == 1);
  CHECK(recorder.state() == PacketSinkState::kStopped);
}

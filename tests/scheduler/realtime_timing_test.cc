#include "streamer/scheduler/realtime_timing.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;
using mw::streamer::RealtimeConfig;
using mw::streamer::internal::RealtimeTiming;

TEST_CASE("Realtime shares the source PTS offset across tracks",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 10000000, 100000).ready);
  const auto audio = timing.Schedule(false, 10020000, 100000);
  REQUIRE_FALSE(audio.ready);
  REQUIRE_FALSE(audio.discard);
  REQUIRE(audio.wake_us == 120000);
  REQUIRE(timing.Schedule(false, 10020000, audio.wake_us).ready);
  const auto video = timing.Schedule(true, 10040000, 120000);
  REQUIRE(video.wake_us == 140000);
  REQUIRE(timing.Schedule(true, 10040000, video.wake_us).ready);
}

TEST_CASE("Realtime waiting does not consume or move its clock mapping",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 10000000, 100000).ready);
  for (std::int64_t now = 100000; now < 140000; now += 1000) {
    const auto decision = timing.Schedule(true, 10040000, now);
    REQUIRE_FALSE(decision.ready);
    REQUIRE_FALSE(decision.discard);
    REQUIRE(decision.wake_us == 140000);
    REQUIRE(decision.wake_us > now);
  }
  REQUIRE(timing.Schedule(true, 10040000, 140000).ready);
}

TEST_CASE(
    "Realtime tolerates minor lateness and optionally discards stale input",
    "[scheduler][realtime]") {
  RealtimeConfig config;
  config.negative_time_discard = 50ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(true, 0, 0).ready);
  REQUIRE(timing.Schedule(true, 40000, 90000).ready);
  const auto stale = timing.Schedule(true, 80000, 130001);
  REQUIRE(stale.discard);
  REQUIRE_FALSE(stale.ready);
  // Discarding must not reanchor the other track's timeline.
  const auto next = timing.Schedule(true, 160000, 140000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 160000);

  RealtimeTiming no_discard(RealtimeConfig{});
  REQUIRE(no_discard.Schedule(true, 0, 0).ready);
  REQUIRE(no_discard.Schedule(true, 40000, 200000).ready);
}

TEST_CASE("Realtime catches up both tracks after processing overload",
          "[scheduler][realtime][recovery]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 0, 0).ready);
  // Video needed 30 ms per 20 ms frame for 100 frames. Ordinary lateness
  // must not move the team's reference or slow the recovering audio track.
  for (std::int64_t index = 1; index <= 100; ++index) {
    const auto late = timing.Schedule(true, index * 20000, index * 30000);
    REQUIRE(late.ready);
    REQUIRE_FALSE(late.resynced);
    const auto audio = timing.Schedule(false, index * 20000, index * 30000);
    REQUIRE(audio.ready);
    REQUIRE_FALSE(audio.resynced);
  }
  // At 15 ms per frame the one-second backlog takes three seconds to drain.
  for (std::int64_t index = 1; index <= 200; ++index) {
    const auto pts = 2000000 + index * 20000;
    const auto now = 3000000 + index * 15000;
    const auto recovered = timing.Schedule(true, pts, now);
    REQUIRE(recovered.ready);
    REQUIRE_FALSE(recovered.resynced);
    REQUIRE(timing.Schedule(false, pts, now).ready);
  }
  // Both tracks resume the original shared realtime deadline after catch-up.
  for (const bool video : {true, false}) {
    const auto next = timing.Schedule(video, 6020000, 6015000);
    REQUIRE_FALSE(next.ready);
    REQUIRE(next.wake_us == 6020000);
    REQUIRE(timing.Schedule(video, 6020000, next.wake_us).ready);
  }
}

TEST_CASE("Realtime discards stale input without reanchoring after overload",
          "[scheduler][realtime]") {
  RealtimeConfig config;
  config.negative_time_discard = 50ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(true, 0, 0).ready);
  const auto late = timing.Schedule(true, 40000, 400000);
  REQUIRE_FALSE(late.ready);
  REQUIRE(late.discard);
  REQUIRE_FALSE(late.resynced);
  const auto next = timing.Schedule(false, 420000, 400000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 420000);
}

TEST_CASE("Realtime distinguishes backward source jumps from a lagging peer",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 1000000, 0).ready);
  REQUIRE(timing.Schedule(false, 1000000, 0).ready);
  REQUIRE(timing.Schedule(false, 2000000, 1000000).ready);
  // A video frame behind audio is not a backward jump on the video track.
  const auto late_video = timing.Schedule(true, 1020000, 1000000);
  REQUIRE(late_video.ready);
  REQUIRE_FALSE(late_video.resynced);
  // An actual backward jump on that same track still reanchors both tracks.
  const auto jump = timing.Schedule(true, 0, 1020000);
  REQUIRE(jump.ready);
  REQUIRE(jump.resynced);
  const auto peer = timing.Schedule(false, 20000, 1020000);
  REQUIRE_FALSE(peer.ready);
  REQUIRE(peer.wake_us == 1040000);
}

TEST_CASE("Realtime forward discontinuity resets the shared team mapping",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 0, 0).ready);
  // Equal to the discontinuity threshold also reanchors.
  REQUIRE(timing.Schedule(true, 1500000, 500000).ready);
  const auto next = timing.Schedule(true, 1540000, 500000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 540000);
}

TEST_CASE("Realtime jitter buffers subsequent frames after anchoring",
          "[scheduler][realtime]") {
  RealtimeConfig config;
  config.jitter_margin = 20ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(true, 1000000, 100000).ready);
  const auto next = timing.Schedule(true, 1040000, 100000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 160000);
  REQUIRE(timing.Schedule(true, 1040000, 160000).ready);

  // Processing lateness keeps the original mapping and jitter margin.
  const auto late = timing.Schedule(true, 1080000, 1000000);
  REQUIRE(late.ready);
  REQUIRE_FALSE(late.resynced);
  REQUIRE(timing.Schedule(true, 1120000, 1000000).ready);
  REQUIRE(timing.Schedule(true, 1920000, 1000000).wake_us == 1040000);
}

TEST_CASE("Realtime fractional FPS deadlines never request an expired timer",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  constexpr std::int64_t kStart = 123456;
  REQUIRE(timing.Schedule(true, 0, kStart).ready);
  for (std::int64_t index = 1; index < 240; ++index) {
    // 30000/1001 FPS rescaled to the clock's whole microseconds.
    const auto pts = (index * 1001000000 + 15000) / 30000;
    const auto due = kStart + pts;
    const auto waiting = timing.Schedule(true, pts, due - 1);
    REQUIRE_FALSE(waiting.ready);
    REQUIRE(waiting.wake_us == due);
    REQUIRE(waiting.wake_us > due - 1);
    REQUIRE(timing.Schedule(true, pts, due).ready);
  }
}

TEST_CASE("Realtime Reset forgets the previous session",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(true, 10000000, 0).ready);
  timing.Reset();
  REQUIRE(timing.Schedule(true, 0, 2000000).ready);
  REQUIRE(timing.Schedule(true, 40000, 2000000).wake_us == 2040000);
}

TEST_CASE("Realtime rejects invalid thresholds", "[scheduler][realtime]") {
  RealtimeConfig config;
  SECTION("negative late tolerance") { config.negative_time_tolerance = -1us; }
  SECTION("zero discontinuity threshold") {
    config.discontinuity_threshold = 0us;
  }
  SECTION("negative jitter") { config.jitter_margin = -1us; }
  SECTION("jitter reaches discontinuity threshold") {
    config.jitter_margin = config.discontinuity_threshold;
  }
  SECTION("negative discard threshold") { config.negative_time_discard = -1us; }
  SECTION("discard threshold reaches resync tolerance") {
    config.negative_time_discard = config.negative_time_tolerance;
  }
  REQUIRE_THROWS_AS(RealtimeTiming{config}, std::invalid_argument);
}

TEST_CASE("Realtime detects overflow without corrupting its mapping",
          "[scheduler][realtime]") {
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE_THROWS_AS(timing.Schedule(true, kMax, -1), std::overflow_error);
  REQUIRE(timing.Schedule(true, 0, 100).ready);
  REQUIRE_THROWS_AS(timing.Schedule(true, kMax, 100), std::overflow_error);
  REQUIRE(timing.Schedule(true, 40000, 100).wake_us == 40100);

  RealtimeTiming difference(RealtimeConfig{});
  REQUIRE(difference.Schedule(true, 0, 0).ready);
  REQUIRE_THROWS_AS(difference.Schedule(true, kMax, kMin), std::overflow_error);
  REQUIRE(difference.Schedule(true, 40000, 0).wake_us == 40000);
}

}  // namespace

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
  REQUIRE(timing.Schedule(10000000, 100000).ready);
  const auto audio = timing.Schedule(10020000, 100000);
  REQUIRE_FALSE(audio.ready);
  REQUIRE_FALSE(audio.discard);
  REQUIRE(audio.wake_us == 120000);
  REQUIRE(timing.Schedule(10020000, audio.wake_us).ready);
  const auto video = timing.Schedule(10040000, 120000);
  REQUIRE(video.wake_us == 140000);
  REQUIRE(timing.Schedule(10040000, video.wake_us).ready);
}

TEST_CASE("Realtime waiting does not consume or move its clock mapping",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(10000000, 100000).ready);
  for (std::int64_t now = 100000; now < 140000; now += 1000) {
    const auto decision = timing.Schedule(10040000, now);
    REQUIRE_FALSE(decision.ready);
    REQUIRE_FALSE(decision.discard);
    REQUIRE(decision.wake_us == 140000);
    REQUIRE(decision.wake_us > now);
  }
  REQUIRE(timing.Schedule(10040000, 140000).ready);
}

TEST_CASE(
    "Realtime tolerates minor lateness and optionally discards stale input",
    "[scheduler][realtime]") {
  RealtimeConfig config;
  config.negative_time_discard = 50ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(0, 0).ready);
  REQUIRE(timing.Schedule(40000, 90000).ready);
  const auto stale = timing.Schedule(80000, 130001);
  REQUIRE(stale.discard);
  REQUIRE_FALSE(stale.ready);
  // Discarding must not reanchor the other track's timeline.
  const auto next = timing.Schedule(160000, 140000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 160000);

  RealtimeTiming no_discard(RealtimeConfig{});
  REQUIRE(no_discard.Schedule(0, 0).ready);
  REQUIRE(no_discard.Schedule(40000, 200000).ready);
}

TEST_CASE("Realtime severe lateness resynchronizes instead of discarding",
          "[scheduler][realtime]") {
  RealtimeConfig config;
  config.negative_time_discard = 50ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(0, 0).ready);
  const auto late = timing.Schedule(40000, 400000);
  REQUIRE(late.ready);
  REQUIRE_FALSE(late.discard);
  // Both tracks inherit the smaller offset; the next track is not accelerated.
  const auto other_track = timing.Schedule(60000, 400000);
  REQUIRE_FALSE(other_track.ready);
  REQUIRE(other_track.wake_us == 420000);
  const auto next = timing.Schedule(80000, 420000);
  REQUIRE(next.wake_us == 440000);
}

TEST_CASE("Realtime forward discontinuity resets the shared team mapping",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(0, 0).ready);
  // Equal to the discontinuity threshold also reanchors.
  REQUIRE(timing.Schedule(1500000, 500000).ready);
  const auto next = timing.Schedule(1540000, 500000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 540000);
}

TEST_CASE("Realtime jitter buffers subsequent frames after anchoring",
          "[scheduler][realtime]") {
  RealtimeConfig config;
  config.jitter_margin = 20ms;
  RealtimeTiming timing(config);
  REQUIRE(timing.Schedule(1000000, 100000).ready);
  const auto next = timing.Schedule(1040000, 100000);
  REQUIRE_FALSE(next.ready);
  REQUIRE(next.wake_us == 160000);
  REQUIRE(timing.Schedule(1040000, 160000).ready);

  // Resynchronization applies the jitter margin again, as realtime does.
  REQUIRE(timing.Schedule(1080000, 1000000).ready);
  REQUIRE(timing.Schedule(1120000, 1000000).wake_us == 1060000);
}

TEST_CASE("Realtime fractional FPS deadlines never request an expired timer",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  constexpr std::int64_t kStart = 123456;
  REQUIRE(timing.Schedule(0, kStart).ready);
  for (std::int64_t index = 1; index < 240; ++index) {
    // 30000/1001 FPS rescaled to the clock's whole microseconds.
    const auto pts = (index * 1001000000 + 15000) / 30000;
    const auto due = kStart + pts;
    const auto waiting = timing.Schedule(pts, due - 1);
    REQUIRE_FALSE(waiting.ready);
    REQUIRE(waiting.wake_us == due);
    REQUIRE(waiting.wake_us > due - 1);
    REQUIRE(timing.Schedule(pts, due).ready);
  }
}

TEST_CASE("Realtime Reset forgets the previous session",
          "[scheduler][realtime]") {
  RealtimeTiming timing(RealtimeConfig{});
  REQUIRE(timing.Schedule(10000000, 0).ready);
  timing.Reset();
  REQUIRE(timing.Schedule(0, 2000000).ready);
  REQUIRE(timing.Schedule(40000, 2000000).wake_us == 2040000);
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
  REQUIRE_THROWS_AS(timing.Schedule(kMax, -1), std::overflow_error);
  REQUIRE(timing.Schedule(0, 100).ready);
  REQUIRE_THROWS_AS(timing.Schedule(kMax, 100), std::overflow_error);
  REQUIRE(timing.Schedule(40000, 100).wake_us == 40100);

  RealtimeTiming difference(RealtimeConfig{});
  REQUIRE(difference.Schedule(0, 0).ready);
  REQUIRE_THROWS_AS(difference.Schedule(kMax, kMin), std::overflow_error);
  REQUIRE(difference.Schedule(40000, 0).wake_us == 40000);
}

}  // namespace

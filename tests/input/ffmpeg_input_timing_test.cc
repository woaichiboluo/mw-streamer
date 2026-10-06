#include "mw/streamer/input/ffmpeg_input_timing.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

namespace {

using mw::streamer::internal::FrameTiming;
using mw::streamer::internal::PlaybackClock;
using namespace std::chrono_literals;

AVFrame MakeFrame(int64_t pts = AV_NOPTS_VALUE, int64_t duration = 0) {
  AVFrame frame{};
  frame.pts = AV_NOPTS_VALUE;
  frame.best_effort_timestamp = pts;
  frame.duration = duration;
  return frame;
}

TEST_CASE("Media timing preserves stream offsets on a shared playback clock",
          "[input][timing]") {
  FrameTiming video({1, 1000}, {25, 1}, false);
  FrameTiming audio({1, 48000}, {0, 1}, true);
  AVFrame video_frame = MakeFrame(5000, 40);
  AVFrame audio_frame = MakeFrame(240960, 1024);
  video.Update(video_frame);
  audio.Update(audio_frame);

  REQUIRE(video.pts_ns() == 5'000'000'000);
  REQUIRE(audio.pts_ns() == 5'020'000'000);
  REQUIRE(video_frame.pts == 5000);
  REQUIRE(audio_frame.pts == 240960);

  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(video.pts_ns(), started);
  REQUIRE(clock.deadline() == started);
  REQUIRE(clock.CanDeliver(video.pts_ns()));
  REQUIRE_FALSE(clock.CanDeliver(audio.pts_ns()));

  clock.Advance(audio.pts_ns());
  REQUIRE(clock.deadline() == started + 20ms);
  REQUIRE(clock.CanDeliver(audio.pts_ns()));
  video_frame = MakeFrame(5040, 40);
  video.Update(video_frame);
  clock.Advance(video.pts_ns());
  REQUIRE(clock.deadline() == started + 40ms);
}

TEST_CASE(
    "Playback clock keeps elapsed scheduling debt through discontinuities",
    "[input][timing]") {
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(5'000'000'000, started);
  clock.Advance(6'000'000'000);
  REQUIRE(clock.deadline() == started + 1s);

  // A late reader must keep this deadline even after a timestamp jump.
  // Resetting it to wall time would conceal its accumulated playback delay.
  clock.Advance(10'000'000'000);
  REQUIRE(clock.deadline() == started + 1s);
  clock.Advance(10'040'000'000);
  REQUIRE(clock.deadline() == started + 1040ms);

  clock.Advance(9'000'000'000);
  REQUIRE(clock.deadline() == started + 1040ms);
  clock.Advance(9'020'000'000);
  REQUIRE(clock.deadline() == started + 1060ms);
  REQUIRE(clock.deadline() < started + 5s);
}

TEST_CASE("Playback discontinuity and track lead limits have strict boundaries",
          "[input][timing]") {
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(0, started);
  clock.Advance(3'000'000'000);
  REQUIRE(clock.deadline() == started + 3s);
  clock.Advance(6'000'000'001);
  REQUIRE(clock.deadline() == started + 3s);

  REQUIRE(clock.CanDeliver(6'000'000'001));
  REQUIRE(clock.CanDeliver(6'000'000'000));
  REQUIRE_FALSE(clock.CanDeliver(6'000'000'002));
  REQUIRE_FALSE(clock.CanDeliver(8'000'000'001));
  REQUIRE(clock.CanDeliver(8'000'000'002));
}

TEST_CASE("Seek changes the media anchor without moving the playback deadline",
          "[input][timing][seek]") {
  const PlaybackClock::Clock::time_point started(10s);
  for (const auto target :
       {6'000'000'000LL, 20'000'000'000LL, 2'000'000'000LL}) {
    PlaybackClock clock(5'000'000'000, started);
    clock.Advance(5'040'000'000);
    const auto deadline = clock.deadline();
    clock.Seek(target);
    REQUIRE(clock.deadline() == deadline);
    REQUIRE(clock.CanDeliver(target));
    REQUIRE_FALSE(clock.CanDeliver(target + 40'000'000));
    clock.Advance(target + 40'000'000);
    REQUIRE(clock.deadline() == deadline + 40ms);
  }
}

TEST_CASE(
    "OBS output timestamps share an epoch and preserve audio video offset",
    "[input][timing][timestamp]") {
  const PlaybackClock::Clock::time_point epoch(2s);
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(5'000'000'000, started, epoch);
  REQUIRE(clock.Timestamp(5'000'000'000) == 8'000'000'000);
  REQUIRE(clock.Timestamp(5'020'000'000) == 8'020'000'000);
  clock.Advance(5'020'000'000);
  REQUIRE(clock.Timestamp(5'020'000'000) == 8'020'000'000);

  // A second input has its own media origin, but the same system epoch.
  PlaybackClock second(100'000'000'000, started + 250ms, epoch);
  REQUIRE(second.Timestamp(100'000'000'000) == 8'250'000'000);
  REQUIRE(second.Timestamp(100'100'000'000) == 8'350'000'000);
}

TEST_CASE("OBS seek preserves output mapping even when timestamps move back",
          "[input][timing][timestamp][seek]") {
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(5'000'000'000, started,
                      PlaybackClock::Clock::time_point(2s));
  clock.Advance(6'000'000'000);
  clock.Seek(20'000'000'000);
  REQUIRE(clock.deadline() == started + 1s);
  REQUIRE(clock.Timestamp(20'000'000'000) == 23'000'000'000);
  clock.Seek(5'000'000'000);
  REQUIRE(clock.Timestamp(5'000'000'000) == 8'000'000'000);
  REQUIRE(clock.deadline() == started + 1s);
  clock.Advance(5'040'000'000);
  REQUIRE(clock.deadline() == started + 1040ms);
  REQUIRE(clock.Timestamp(5'040'000'000) == 8'040'000'000);
}

TEST_CASE("OBS first playback sleep does not change the prepared output anchor",
          "[input][timing][timestamp]") {
  const PlaybackClock::Clock::time_point prepared(10s);
  PlaybackClock clock(5'000'000'000, prepared,
                      PlaybackClock::Clock::time_point(2s));
  clock.StartDelivery(prepared + 250ms);
  REQUIRE(clock.Timestamp(5'000'000'000) == 8'000'000'000);
  REQUIRE(clock.deadline() == prepared + 250ms);
  clock.Advance(5'040'000'000);
  REQUIRE(clock.deadline() == prepared + 290ms);
  REQUIRE(clock.Timestamp(5'040'000'000) == 8'040'000'000);
}

TEST_CASE("OBS loops accumulate absolute media end and retain clock debt",
          "[input][timing][timestamp][loop]") {
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(5'000'000'000, started,
                      PlaybackClock::Clock::time_point(2s));
  for (int64_t cycle = 0; cycle < 3; ++cycle) {
    CAPTURE(cycle);
    REQUIRE(clock.Timestamp(5'000'000'000) ==
            8'000'000'000 + cycle * 15'000'000'000);
    REQUIRE(clock.deadline() == started + cycle * 10s);
    for (int second = 6; second <= 14; ++second) {
      clock.Advance(second * 1'000'000'000LL);
    }
    clock.Advance(14'500'000'000);
    REQUIRE(clock.LoopDeadline(15'000'000'000) == started + (cycle + 1) * 10s);
    clock.EndLoop(15'000'000'000);
    clock.BeginLoop(5'000'000'000);
    REQUIRE(clock.deadline() == started + (cycle + 1) * 10s);
  }
}

TEST_CASE("OBS EOF offset preserves lateness and leading track timestamps",
          "[input][timing][timestamp][loop]") {
  const PlaybackClock::Clock::time_point started(10s);
  PlaybackClock clock(5'000'000'000, started);
  // A leading frame may be released early; EOF still uses absolute next_pts,
  // rather than just the frame's duration added to its delivery deadline.
  clock.Advance(14'000'000'000);
  clock.Advance(14'500'000'000);
  REQUIRE(clock.LoopDeadline(15'000'000'000) == started + 1s);
  clock.EndLoop(15'000'000'000);
  clock.BeginLoop(5'000'000'000);
  REQUIRE(clock.deadline() == started + 1s);
  REQUIRE(clock.Timestamp(5'000'000'000) == 25'000'000'000);
  clock.Advance(5'040'000'000);
  REQUIRE(clock.deadline() == started + 1040ms);
}

TEST_CASE("OBS reconnect creates a fresh mapping on the shared system epoch",
          "[input][timing][timestamp][reconnect]") {
  const PlaybackClock::Clock::time_point epoch(2s);
  PlaybackClock old(5'000'000'000, PlaybackClock::Clock::time_point(10s),
                    epoch);
  old.Advance(6'000'000'000);
  old.EndLoop(7'000'000'000);
  old.BeginLoop(5'000'000'000);
  REQUIRE(old.Timestamp(5'000'000'000) == 15'000'000'000);
  // Network reconnection is a new media instance, not a loop of the old one.
  PlaybackClock reconnected(50'000'000'000,
                            PlaybackClock::Clock::time_point(14s), epoch);
  REQUIRE(reconnected.Timestamp(50'000'000'000) == 12'000'000'000);
  REQUIRE(reconnected.deadline() == PlaybackClock::Clock::time_point(14s));
}

TEST_CASE(
    "Timing flush clears pre-seek predictions and keeps duration fallback",
    "[input][timing][seek]") {
  FrameTiming timing({1, 1000}, {25, 1}, false);
  AVFrame frame = MakeFrame(5000, 50);
  timing.Update(frame);
  timing.Flush();
  frame = MakeFrame();
  timing.Update(frame);
  REQUIRE(frame.pts == 0);
  REQUIRE(frame.duration == 50);
  timing.Flush();
  frame = MakeFrame(2000);
  timing.Update(frame);
  REQUIRE(frame.pts == 2000);
  REQUIRE(frame.duration == 50);
  frame = MakeFrame();
  timing.Update(frame);
  REQUIRE(frame.pts == 2050);
}

TEST_CASE("Loop reopening retains OBS video duration estimation",
          "[input][timing][loop]") {
  FrameTiming old({1, 1000}, {25, 1}, false);
  AVFrame frame = MakeFrame(1000, 100);
  old.Update(frame);
  // Reset clears predicted media PTS but keeps last_duration in OBS. A new
  // decoder here must carry that estimate, not fall back to nominal 40 ms.
  FrameTiming reopened({1, 90000}, {25, 1}, false, old.duration_ns());
  frame = MakeFrame(450000);
  reopened.Update(frame);
  REQUIRE(frame.duration == 9000);
  REQUIRE(reopened.next_pts_ns() == 5'100'000'000);
  frame = MakeFrame();
  reopened.Update(frame);
  REQUIRE(frame.pts == 459000);
  REQUIRE(frame.duration == 9000);
  // A fresh connection has no estimate inherited from the disconnected one.
  FrameTiming reconnected({1, 90000}, {25, 1}, false);
  frame = MakeFrame(450000);
  reconnected.Update(frame);
  REQUIRE(frame.duration == 3600);
}

TEST_CASE("Variable video timing uses timestamp deltas before estimating rate",
          "[input][timing]") {
  FrameTiming timing({1, 1000}, {25, 1}, false);
  AVFrame frame = MakeFrame(0);
  timing.Update(frame);
  REQUIRE(frame.duration == 40);

  frame = MakeFrame(40);
  timing.Update(frame);
  REQUIRE(frame.duration == 40);
  frame = MakeFrame(140);
  timing.Update(frame);
  REQUIRE(frame.duration == 100);

  frame = MakeFrame();
  timing.Update(frame);
  REQUIRE(timing.pts_ns() == 240'000'000);
  REQUIRE(frame.pts == 240);
  REQUIRE(frame.duration == 100);
}

TEST_CASE(
    "Missing audio timestamps accumulate samples without millisecond drift",
    "[input][timing]") {
  FrameTiming timing({1, 1000}, {0, 1}, true);
  constexpr int64_t kPacketDurationNs = 21'333'333;
  constexpr int64_t kPackets = 1500;
  int64_t last_pts_ns = 0;
  int64_t last_pts = 0;
  for (int64_t packet = 0; packet < kPackets; ++packet) {
    AVFrame frame = MakeFrame();
    frame.nb_samples = 1024;
    frame.sample_rate = 48000;
    timing.Update(frame);
    REQUIRE(timing.pts_ns() == packet * kPacketDurationNs);
    last_pts_ns = timing.pts_ns();
    last_pts = frame.pts;
  }

  // 1500 AAC packets span 32 seconds. Summing rounded 21 ms durations loses
  // roughly half a second; nanosecond accumulation loses less than 1 us.
  REQUIRE(last_pts_ns + kPacketDurationNs == 31'999'999'500);
  REQUIRE(32'000'000'000 - (last_pts_ns + kPacketDurationNs) < 1000);
  REQUIRE(last_pts > (kPackets - 1) * 21 + 490);
}

TEST_CASE("Recovered best effort timestamps reanchor missing audio timing",
          "[input][timing]") {
  FrameTiming timing({1, 1000}, {0, 1}, true);
  for (int64_t packet = 0; packet < 2; ++packet) {
    AVFrame frame = MakeFrame();
    frame.nb_samples = 1024;
    frame.sample_rate = 48000;
    timing.Update(frame);
    REQUIRE(timing.pts_ns() == packet * 21'333'333);
  }

  AVFrame recovered = MakeFrame(1000);
  recovered.pts = 9999;
  recovered.nb_samples = 1024;
  recovered.sample_rate = 48000;
  timing.Update(recovered);
  REQUIRE(timing.pts_ns() == 1'000'000'000);
  REQUIRE(recovered.pts == 1000);

  AVFrame missing = MakeFrame();
  missing.pts = 9999;
  missing.nb_samples = 1024;
  missing.sample_rate = 48000;
  timing.Update(missing);
  REQUIRE(timing.pts_ns() == 1'021'333'333);
  REQUIRE(missing.pts == 1021);
}

TEST_CASE("Decoded frame duration takes priority over samples and frame rate",
          "[input][timing]") {
  FrameTiming video({1, 1000}, {25, 1}, false);
  AVFrame frame = MakeFrame(10, 50);
  video.Update(frame);
  REQUIRE(frame.duration == 50);
  frame = MakeFrame();
  video.Update(frame);
  REQUIRE(frame.pts == 60);
  REQUIRE(frame.duration == 50);
  frame = MakeFrame(100, 25);
  video.Update(frame);
  frame = MakeFrame();
  video.Update(frame);
  REQUIRE(frame.pts == 125);

  FrameTiming audio({1, 1000}, {0, 1}, true);
  frame = MakeFrame(10, 50);
  frame.nb_samples = 1024;
  frame.sample_rate = 48000;
  audio.Update(frame);
  frame = MakeFrame();
  frame.nb_samples = 1024;
  frame.sample_rate = 48000;
  audio.Update(frame);
  REQUIRE(frame.pts == 60);
  REQUIRE(frame.duration == 21);
}

TEST_CASE("Video timing estimates the frame rate only when timing is absent",
          "[input][timing]") {
  FrameTiming timing({1, 90000}, {30000, 1001}, false);
  AVFrame first = MakeFrame();
  timing.Update(first);
  REQUIRE(first.pts == 0);
  REQUIRE(first.duration == 3003);
  AVFrame next = MakeFrame();
  timing.Update(next);
  REQUIRE(timing.pts_ns() == 33'366'667);
  REQUIRE(next.pts == 3003);
  REQUIRE(next.duration == 3003);
}

TEST_CASE("Video timing preserves zero and negative timestamp deltas",
          "[input][timing]") {
  SECTION("Backward timestamps keep their measured negative duration") {
    FrameTiming timing({1, 1000}, {25, 1}, false);
    AVFrame frame = MakeFrame(1000);
    timing.Update(frame);
    REQUIRE(frame.duration == 40);
    frame = MakeFrame(960);
    timing.Update(frame);
    REQUIRE(frame.duration == -40);
    frame = MakeFrame();
    timing.Update(frame);
    REQUIRE(frame.pts == 920);
    REQUIRE(frame.duration == -40);
    REQUIRE(timing.pts_ns() == 920'000'000);
  }

  SECTION("Repeated timestamps keep zero duration instead of estimating rate") {
    FrameTiming timing({1, 1000}, {25, 1}, false);
    AVFrame frame = MakeFrame(10);
    timing.Update(frame);
    REQUIRE(frame.duration == 40);
    frame = MakeFrame(10);
    timing.Update(frame);
    REQUIRE(frame.duration == 0);
    frame = MakeFrame();
    timing.Update(frame);
    REQUIRE(frame.pts == 10);
    REQUIRE(frame.duration == 0);
    REQUIRE(timing.pts_ns() == 10'000'000);
  }

  SECTION("Explicit negative duration takes priority over the nominal rate") {
    FrameTiming timing({1, 1000}, {25, 1}, false);
    AVFrame frame = MakeFrame(1000, -10);
    timing.Update(frame);
    REQUIRE(frame.duration == -10);
    frame = MakeFrame();
    timing.Update(frame);
    REQUIRE(frame.pts == 990);
    REQUIRE(frame.duration == -10);
    REQUIRE(timing.pts_ns() == 990'000'000);
  }
}

TEST_CASE("Frames without any timing information stay at zero without throwing",
          "[input][timing]") {
  FrameTiming video({1, 1000}, {0, 1}, false);
  FrameTiming audio({1, 1000}, {0, 1}, true);
  for (int attempt = 0; attempt < 3; ++attempt) {
    AVFrame video_frame = MakeFrame();
    AVFrame audio_frame = MakeFrame();
    REQUIRE_NOTHROW(video.Update(video_frame));
    REQUIRE_NOTHROW(audio.Update(audio_frame));
    REQUIRE(video.pts_ns() == 0);
    REQUIRE(audio.pts_ns() == 0);
    REQUIRE(video_frame.pts == 0);
    REQUIRE(audio_frame.pts == 0);
    REQUIRE(video_frame.duration == 0);
    REQUIRE(audio_frame.duration == 0);
  }
}

}  // namespace

#include "streamer/input/timestamp_discontinuity.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using mw::streamer::AvInputInfo;
using mw::streamer::internal::TimestampDiscontinuity;
namespace ffmpeg = mw::streamer::ffmpeg;

constexpr std::int64_t kSecond = 1000000;
constexpr std::int64_t kFrame = 20000;
constexpr std::int64_t kThirtyHours = 30LL * 60 * 60 * kSecond;

AvInputInfo MakeInfo(bool discontinuous = true) {
  AvInputInfo info;
  info.timestamp_discontinuities = discontinuous;
  ffmpeg::StreamInfo video;
  video.stream_index = 0;
  video.time_base = {1, 1000000};
  video.codec_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  info.streams.push_back(std::move(video));
  ffmpeg::StreamInfo audio;
  audio.stream_index = 1;
  audio.time_base = {1, 1000000};
  audio.codec_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  info.streams.push_back(std::move(audio));
  return info;
}

ffmpeg::Packet MakePacket(std::int64_t dts, std::int64_t pts,
                          std::int64_t duration = kFrame, int stream = 0) {
  ffmpeg::Packet packet;
  packet->stream_index = stream;
  packet->time_base = {1, 1000000};
  packet->dts = dts;
  packet->pts = pts;
  packet->duration = duration;
  return packet;
}

void Seed(TimestampDiscontinuity& normalizer, std::int64_t dts = 0) {
  auto packet = MakePacket(dts, dts);
  normalizer.Process(packet);
}

TEST_CASE("Timestamp jump threshold is strictly greater than ten seconds",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer);
  SECTION("Exactly ten seconds is retained") {
    auto packet = MakePacket(kFrame + 10 * kSecond, 10 * kSecond);
    normalizer.Process(packet);
    CHECK(packet->dts == kFrame + 10 * kSecond);
    CHECK(packet->pts == 10 * kSecond);
  }
  SECTION("One microsecond beyond ten seconds is corrected") {
    auto packet = MakePacket(kFrame + 10 * kSecond + 1, 10 * kSecond + 1);
    normalizer.Process(packet);
    CHECK(packet->dts == kFrame);
    CHECK(packet->pts == 0);
  }
}

TEST_CASE("Backward tolerance compares against the previous DTS",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer, kSecond);
  SECTION("Exactly one hundred milliseconds backward is retained") {
    auto packet = MakePacket(900000, 880000);
    normalizer.Process(packet);
    CHECK(packet->dts == 900000);
    CHECK(packet->pts == 880000);
  }
  SECTION("Larger backward movement is corrected to predicted DTS") {
    auto packet = MakePacket(899999, 879999);
    normalizer.Process(packet);
    CHECK(packet->dts == kSecond + kFrame);
    CHECK(packet->pts == kSecond);
  }
}

TEST_CASE("Correction preserves reordered negative PTS and future offsets",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer);
  auto jump = MakePacket(20 * kSecond, 20 * kSecond - 2 * kFrame);
  normalizer.Process(jump);
  CHECK(jump->dts == kFrame);
  CHECK(jump->pts == -kFrame);
  auto following = MakePacket(20 * kSecond + kFrame, 20 * kSecond);
  normalizer.Process(following);
  CHECK(following->dts == 2 * kFrame);
  CHECK(following->pts == kFrame);
}

TEST_CASE("A shared correction applies to an existing audio track",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer);
  auto audio = MakePacket(0, 0, kFrame, 1);
  normalizer.Process(audio);
  auto jump = MakePacket(20 * kSecond, 20 * kSecond);
  normalizer.Process(jump);
  auto audio_after_jump = MakePacket(20 * kSecond, 20 * kSecond, kFrame, 1);
  normalizer.Process(audio_after_jump);
  CHECK(audio_after_jump->dts == kFrame);
  CHECK(audio_after_jump->pts == kFrame);
}

TEST_CASE("The first packet of another track uses global last DTS",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer, kSecond);
  SECTION("A normal interstream offset survives") {
    auto audio = MakePacket(kSecond + 50000, kSecond + 50000, kFrame, 1);
    normalizer.Process(audio);
    CHECK(audio->dts == kSecond + 50000);
  }
  SECTION("A large interstream offset is corrected") {
    auto audio = MakePacket(20 * kSecond, 20 * kSecond, kFrame, 1);
    normalizer.Process(audio);
    CHECK(audio->dts == kSecond);
    auto video = MakePacket(20 * kSecond + kFrame, 20 * kSecond + kFrame);
    normalizer.Process(video);
    CHECK(video->dts == kSecond + kFrame);
  }
}

TEST_CASE("Missing DTS is not synthesized and existing offset still shifts PTS",
          "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo());
  Seed(normalizer);
  auto jump = MakePacket(20 * kSecond, 20 * kSecond);
  normalizer.Process(jump);
  auto missing = MakePacket(AV_NOPTS_VALUE, 20 * kSecond + kFrame);
  normalizer.Process(missing);
  CHECK(missing->dts == AV_NOPTS_VALUE);
  CHECK(missing->pts == 2 * kFrame);
  auto next = MakePacket(20 * kSecond + 2 * kFrame, 20 * kSecond + 2 * kFrame);
  normalizer.Process(next);
  CHECK(next->dts == 3 * kFrame);
}

TEST_CASE(
    "Continuous formats independently discard timestamps beyond thirty hours",
    "[timestamp_discontinuity]") {
  TimestampDiscontinuity normalizer(MakeInfo(false));
  Seed(normalizer);
  SECTION("The exact threshold is retained") {
    auto packet = MakePacket(kFrame + kThirtyHours, kFrame + kThirtyHours);
    normalizer.Process(packet);
    CHECK(packet->dts == kFrame + kThirtyHours);
    CHECK(packet->pts == kFrame + kThirtyHours);
  }
  SECTION("Invalid DTS does not discard valid PTS") {
    auto packet = MakePacket(kFrame + kThirtyHours + 1, kFrame);
    normalizer.Process(packet);
    CHECK(packet->dts == AV_NOPTS_VALUE);
    CHECK(packet->pts == kFrame);
  }
  SECTION("Invalid PTS does not discard valid DTS") {
    auto packet = MakePacket(kFrame, kFrame - kThirtyHours - 1);
    normalizer.Process(packet);
    CHECK(packet->dts == kFrame);
    CHECK(packet->pts == AV_NOPTS_VALUE);
  }
}

TEST_CASE("DTS prediction follows packet duration and audio codec frame size",
          "[timestamp_discontinuity]") {
  auto info = MakeInfo();
  SECTION("Video packet duration takes precedence over frame rate") {
    info.streams[0].frame_rate = {50, 1};
    TimestampDiscontinuity normalizer(info);
    auto first = MakePacket(0, 0, 40000);
    normalizer.Process(first);
    auto jump = MakePacket(20 * kSecond, 20 * kSecond);
    normalizer.Process(jump);
    CHECK(jump->dts == 40000);
  }
  SECTION("Video frame rate predicts a missing packet duration") {
    info.streams[0].frame_rate = {50, 1};
    TimestampDiscontinuity normalizer(info);
    auto first = MakePacket(0, 0, 0);
    normalizer.Process(first);
    auto jump = MakePacket(20 * kSecond, 20 * kSecond);
    normalizer.Process(jump);
    CHECK(jump->dts == kFrame);
  }
  SECTION("Audio codec frame size takes precedence over packet duration") {
    auto* parameters = info.streams[1].codec_parameters.get();
    parameters->sample_rate = 48000;
    parameters->frame_size = 1024;
    TimestampDiscontinuity normalizer(info);
    auto first = MakePacket(0, 0, 40000, 1);
    normalizer.Process(first);
    auto jump = MakePacket(20 * kSecond, 20 * kSecond, 0, 1);
    normalizer.Process(jump);
    CHECK(jump->dts == 21333);
  }
  SECTION("Audio with a zero codec frame size does not use packet duration") {
    info.streams[1].codec_parameters.get()->sample_rate = 48000;
    TimestampDiscontinuity normalizer(info);
    auto first = MakePacket(0, 0, 40000, 1);
    normalizer.Process(first);
    auto jump = MakePacket(20 * kSecond, 20 * kSecond, 0, 1);
    normalizer.Process(jump);
    CHECK(jump->dts == 0);
  }
}

TEST_CASE("Native ninety kilohertz timestamps retain PTS DTS spacing",
          "[timestamp_discontinuity]") {
  auto info = MakeInfo();
  info.streams[0].time_base = {1, 90000};
  TimestampDiscontinuity normalizer(info);
  auto first = MakePacket(0, 3600, 1800);
  first->time_base = {1, 90000};
  normalizer.Process(first);
  auto jump = MakePacket(1800000, 1803600, 1800);
  jump->time_base = {1, 90000};
  normalizer.Process(jump);
  CHECK(jump->dts == 1800);
  CHECK(jump->pts == 5400);
  CHECK(jump->pts - jump->dts == 3600);
}

TEST_CASE("Initial missing DTS prediction accounts for video reorder delay",
          "[timestamp_discontinuity]") {
  auto info = MakeInfo();
  info.streams[0].frame_rate = {50, 1};
  info.streams[0].codec_parameters.get()->video_delay = 2;
  TimestampDiscontinuity normalizer(info);
  auto first = MakePacket(AV_NOPTS_VALUE, kSecond);
  normalizer.Process(first);
  CHECK(first->dts == AV_NOPTS_VALUE);
  CHECK(first->pts == kSecond);
  auto jump = MakePacket(20 * kSecond, 20 * kSecond);
  normalizer.Process(jump);
  CHECK(jump->dts == kSecond - kFrame);
}

TEST_CASE("A new generation starts without the previous correction",
          "[timestamp_discontinuity]") {
  const auto info = MakeInfo();
  TimestampDiscontinuity previous(info);
  Seed(previous);
  auto jump = MakePacket(20 * kSecond, 20 * kSecond);
  previous.Process(jump);
  REQUIRE(jump->dts == kFrame);
  TimestampDiscontinuity next(info);
  auto first = MakePacket(20 * kSecond, 20 * kSecond);
  next.Process(first);
  CHECK(first->dts == 20 * kSecond);
}

TEST_CASE("Timestamp arithmetic overflow throws without wrapping",
          "[timestamp_discontinuity]") {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  SECTION("Prediction cannot pass the maximum timestamp") {
    TimestampDiscontinuity normalizer(MakeInfo());
    auto packet = MakePacket(kMax - 1, kMax - 1);
    CHECK_THROWS_AS(normalizer.Process(packet), std::overflow_error);
  }
  SECTION("A correction cannot overflow PTS") {
    TimestampDiscontinuity normalizer(MakeInfo());
    Seed(normalizer, 20 * kSecond);
    auto packet = MakePacket(0, kMax - 1);
    CHECK_THROWS_AS(normalizer.Process(packet), std::overflow_error);
  }
}

}  // namespace

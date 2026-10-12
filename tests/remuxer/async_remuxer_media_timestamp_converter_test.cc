#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "../encoder/encoder_test_support.h"
#include "mw/streamer/remuxer/media_timestamp_converter.h"

namespace {
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::MediaTimestampConverter;
ffmpeg::StreamInfo Video() {
  auto stream = encoder_test::VideoStream();
  stream.time_base = {1, 25};
  return stream;
}
ffmpeg::StreamInfo Audio() {
  auto stream = encoder_test::AudioStream();
  stream.time_base = {1, 48000};
  return stream;
}
ffmpeg::Packet Packet(const ffmpeg::StreamInfo& stream, std::int64_t pts,
                      std::int64_t dts) {
  ffmpeg::Packet packet;
  packet->stream_index = stream.stream_index;
  packet->time_base = stream.time_base;
  packet->pts = pts;
  packet->dts = dts;
  return packet;
}
TEST_CASE("共同毫秒偏移保留B帧负DTS与AAC priming关系",
          "[remuxer][async][timestamp]") {
  const auto video = Video();
  const auto audio = Audio();
  MediaTimestampConverter converter({video, audio});
  auto v = Packet(video, 0, -2);
  auto a = Packet(audio, -1024, -1024);
  converter.Initialize({v, a});
  CHECK(converter.Convert(v).pts_ms == 80);
  CHECK(converter.Convert(v).dts_ms == 0);
  CHECK(converter.Convert(a).pts_ms == 59);
  CHECK(converter.Convert(a).dts_ms == 59);
  CHECK(converter.Convert(Packet(video, 3, -1)).pts_ms == 200);
  CHECK(converter.Convert(Packet(video, 1, 0)).pts_ms == 120);
  CHECK(v->pts == 0);
  CHECK(v->dts == -2);
  CHECK(a->dts == -1024);
}
TEST_CASE("正起点不各轨归零且同一偏移不随后续包改变",
          "[remuxer][async][timestamp]") {
  const auto video = Video();
  const auto audio = Audio();
  MediaTimestampConverter converter({video, audio});
  converter.Initialize({Packet(video, 100, 98), Packet(audio, 48000, 48000)});
  CHECK(converter.Convert(Packet(video, 100, 98)).pts_ms == 4000);
  CHECK(converter.Convert(Packet(video, 100, 98)).dts_ms == 3920);
  CHECK(converter.Convert(Packet(audio, 48000, 48000)).pts_ms == 1000);
  converter.Initialize({Packet(video, -10, -10)});
  CHECK_THROWS_AS(converter.Convert(Packet(video, -1, -1)),
                  std::invalid_argument);
}
TEST_CASE("单音轨共同偏移保留priming之后的样本跨度",
          "[remuxer][async][timestamp]") {
  const auto stream = Audio();
  MediaTimestampConverter converter({stream});
  converter.Initialize({Packet(stream, -1024, -1024)});
  CHECK(converter.Convert(Packet(stream, -1024, -1024)).dts_ms == 0);
  CHECK(converter.Convert(Packet(stream, 0, 0)).dts_ms == 21);
  CHECK(converter.Convert(Packet(stream, 48000, 48000)).dts_ms == 1021);
}
TEST_CASE("非整数时间基直接换算毫秒没有累积舍入",
          "[remuxer][async][timestamp]") {
  auto stream = Video();
  stream.time_base = {1001, 30000};
  MediaTimestampConverter converter({stream});
  converter.Initialize({Packet(stream, 0, 0)});
  for (std::int64_t i = 0; i < 10001; ++i) {
    auto packet = Packet(stream, i, i);
    packet->time_base = {2002, 60000};
    CHECK(converter.Convert(packet).pts_ms ==
          static_cast<std::uint64_t>((i * 1001000 + 15000) / 30000));
  }
}
TEST_CASE("非法媒体包不能建立共同偏移", "[remuxer][async][timestamp]") {
  const auto stream = Video();
  MediaTimestampConverter converter({stream});
  auto packet = Packet(stream, 0, 0);
  SECTION("缺PTS") { packet->pts = AV_NOPTS_VALUE; }
  SECTION("缺DTS") { packet->dts = AV_NOPTS_VALUE; }
  SECTION("未知轨道") { packet->stream_index = 99; }
  SECTION("不匹配时间基") { packet->time_base = {1, 50}; }
  CHECK_THROWS_AS(converter.Initialize({packet}), std::invalid_argument);
  converter.Initialize({Packet(stream, 0, -2)});
  CHECK(converter.Convert(Packet(stream, 0, -2)).dts_ms == 0);
}
TEST_CASE("毫秒转换溢出明确失败", "[remuxer][async][timestamp]") {
  auto stream = Video();
  stream.time_base = {1000, 1};
  MediaTimestampConverter converter({stream});
  CHECK_THROWS_AS(converter.Initialize({Packet(
                      stream, std::numeric_limits<std::int64_t>::max(), 0)}),
                  std::overflow_error);
  converter.Initialize({Packet(stream, 0, 0)});
  CHECK(converter.Convert(Packet(stream, 0, 0)).pts_ms == 0);
}
}  // namespace

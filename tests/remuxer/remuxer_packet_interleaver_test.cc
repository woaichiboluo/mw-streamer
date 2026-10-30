#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "remuxer_test_support.h"
#include "streamer/remuxer/packet_interleaver.h"

namespace {
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::PacketInterleaver;
ffmpeg::StreamInfo Video() {
  auto stream = remuxer_test::VideoStream();
  stream.time_base = {1001, 30000};
  return stream;
}
ffmpeg::StreamInfo Audio() {
  auto stream = remuxer_test::AudioStream();
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
TEST_CASE("原生DTS交织保留全部B帧与AAC负起点", "[remuxer][async][interleave]") {
  const auto v = Video();
  const auto a = Audio();
  PacketInterleaver interleaver({v, a});
  auto video = Packet(v, 0, -2);
  auto audio = Packet(a, -1024, -1024);
  interleaver.Push(video);
  CHECK(interleaver.PopReady().empty());
  interleaver.Push(audio);
  const auto first = interleaver.FirstPackets();
  REQUIRE(first.size() == 2);
  CHECK(first[0]->dts == -2);
  CHECK(first[1]->dts == -1024);
  auto ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->dts == -2);
  CHECK(ready.front().packet->pts == 0);
  CHECK(video->dts == -2);
  interleaver.Push(Packet(v, 3, -1));
  interleaver.Push(Packet(a, 0, 0));
  ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->stream_index == v.stream_index);
  auto tail = interleaver.PopReady(true);
  REQUIRE(tail.size() == 2);
  CHECK(tail.front().packet->dts == -1024);
  CHECK(tail.back().packet->dts == 0);
}
TEST_CASE("双轨原生时间基交织输出单调且保留正起点差",
          "[remuxer][async][interleave]") {
  const auto v = Video();
  const auto a = Audio();
  PacketInterleaver interleaver({v, a});
  std::vector<ffmpeg::Packet> output;
  const auto append = [&](std::vector<PacketInterleaver::Item> packets) {
    for (auto& packet : packets) output.push_back(std::move(packet.packet));
  };
  for (int i = 0; i < 300; ++i) {
    interleaver.Push(Packet(v, 300 + i, 300 + i));
    append(interleaver.PopReady());
    interleaver.Push(Packet(a, 480000 + i * 1024, 480000 + i * 1024));
    append(interleaver.PopReady());
  }
  append(interleaver.PopReady(true));
  REQUIRE(output.size() == 600);
  for (std::size_t i = 1; i < output.size(); ++i)
    CHECK(av_compare_ts(output[i - 1]->dts, output[i - 1]->time_base,
                        output[i]->dts, output[i]->time_base) <= 0);
  CHECK(output.front()->stream_index == a.stream_index);
  CHECK(output.front()->dts == 480000);
  CHECK(interleaver.queued_packets() == 0);
}
TEST_CASE("非关键视频首包不被筛除", "[remuxer][async][interleave]") {
  const auto v = Video();
  PacketInterleaver interleaver({v});
  auto packet = Packet(v, 12, 10);
  REQUIRE((packet->flags & AV_PKT_FLAG_KEY) == 0);
  interleaver.Push(packet);
  auto ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->pts == 12);
  CHECK(ready.front().packet->dts == 10);
}
TEST_CASE("缺轨EOF排空全部已接收包", "[remuxer][async][interleave]") {
  const auto v = Video();
  PacketInterleaver interleaver({v, Audio()});
  for (int i = 0; i < 100; ++i) interleaver.Push(Packet(v, i, i));
  CHECK(interleaver.PopReady().empty());
  auto ready = interleaver.PopReady(true);
  REQUIRE(ready.size() == 100);
  CHECK_THROWS_AS(interleaver.Push(Packet(v, 100, 100)), std::logic_error);
}
TEST_CASE("ZLM启动缺轨超过十秒明确失败", "[remuxer][async][interleave]") {
  const auto v = Video();
  PacketInterleaver interleaver({v, Audio()});
  for (int i = 0; i <= 400; ++i) interleaver.Push(Packet(v, i, i));
  CHECK_THROWS_AS(interleaver.PopReady(), std::runtime_error);
  CHECK(interleaver.PopReady(true).size() == 401);
}
TEST_CASE("首包齐全后缺轨超过FFmpeg十秒窗口允许进度",
          "[remuxer][async][interleave]") {
  const auto v = Video();
  PacketInterleaver interleaver({v, Audio()});
  interleaver.Push(Packet(Audio(), 0, 0));
  for (int i = 0; i <= 400; ++i) interleaver.Push(Packet(v, i, i));
  auto ready = interleaver.PopReady();
  CHECK_FALSE(ready.empty());
  CHECK(interleaver.queued_packets() <= 301);
  CHECK(ready.size() + interleaver.queued_packets() == 402);
}
TEST_CASE("交织拒绝轨道DTS回退而PTS允许B帧回退",
          "[remuxer][async][interleave]") {
  const auto v = Video();
  PacketInterleaver interleaver({v});
  interleaver.Push(Packet(v, 3, 0));
  interleaver.Push(Packet(v, 1, 1));
  CHECK_THROWS_AS(interleaver.Push(Packet(v, 4, 0)), std::invalid_argument);
  CHECK(interleaver.PopReady(true).size() == 2);
}
TEST_CASE("等DTS首包按输出轨道创建顺序释放而不等待更晚包",
          "[remuxer][async][interleave]") {
  auto video = Video();
  auto audio = Audio();
  // Input indices need not match output stream creation order.
  video.stream_index = 9;
  audio.stream_index = 1;
  PacketInterleaver interleaver({video, audio});
  interleaver.Push(Packet(audio, 0, 0));
  CHECK(interleaver.PopReady().empty());
  interleaver.Push(Packet(video, 0, 0));
  auto ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->stream_index == video.stream_index);
  CHECK(interleaver.queued_packets() == 1);
  interleaver.Push(Packet(video, 1, 1));
  ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->stream_index == audio.stream_index);
}
TEST_CASE("历史较晚DTS不能代替当前轨道缓存", "[remuxer][async][interleave]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  interleaver.Push(Packet(video, 0, 0));
  interleaver.Push(Packet(audio, 4800, 4800));
  auto ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->stream_index == video.stream_index);
  interleaver.Push(Packet(video, 1, 1));
  ready = interleaver.PopReady();
  REQUIRE(ready.size() == 1);
  CHECK(ready.front().packet->stream_index == video.stream_index);
  // Only audio remains: wait even though a video packet once arrived.
  CHECK(interleaver.PopReady().empty());
  auto tail = interleaver.PopReady(true);
  REQUIRE(tail.size() == 1);
  CHECK(tail.front().packet->stream_index == audio.stream_index);
}

}  // namespace

#include "mw/streamer/remuxer/packet_interleaver.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "../encoder/encoder_test_support.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::PacketInterleaver;

ffmpeg::StreamInfo Video(AVRational fps = {50, 1}) {
  auto stream = encoder_test::VideoStream();
  stream.time_base = av_inv_q(fps);
  stream.codec_parameters.get()->framerate = fps;
  return stream;
}

ffmpeg::StreamInfo Audio() {
  auto stream = encoder_test::AudioStream();
  stream.time_base = {1, 48000};
  return stream;
}

ffmpeg::Packet Packet(const ffmpeg::StreamInfo& stream, std::int64_t pts,
                      std::int64_t dts, int id = 0, bool key = true,
                      int bytes = 0) {
  ffmpeg::Packet packet;
  if (bytes > 0) REQUIRE(av_new_packet(packet.get(), bytes) == 0);
  packet->stream_index = stream.stream_index;
  packet->time_base = stream.time_base;
  packet->pts = pts;
  packet->dts = dts;
  packet->pos = id;
  packet->duration =
      stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO ? 1
                                                                      : 1024;
  packet->flags = key ? AV_PKT_FLAG_KEY : 0;
  return packet;
}

void Append(std::vector<PacketInterleaver::Item>& output,
            std::vector<PacketInterleaver::Item> packets) {
  output.insert(output.end(), std::make_move_iterator(packets.begin()),
                std::make_move_iterator(packets.end()));
}

TEST_CASE("交织队列统计区分启动筛选与正常出队并保留入队时刻",
          "[remuxer][interleave][performance]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  const auto arrival =
      std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
  interleaver.Push(Packet(audio, 0, 0, 0, true, 7), 1000000000, arrival);
  CHECK(interleaver.queued_packets() == 1);
  CHECK(interleaver.queued_bytes() == 7);
  interleaver.Push(Packet(video, 1, 1, 1, false, 11), 1010000000, arrival);
  CHECK(interleaver.queued_packets() == 0);
  CHECK(interleaver.queued_bytes() == 0);
  CHECK(interleaver.discarded_packets() == 2);
  CHECK(interleaver.discarded_bytes() == 18);
  interleaver.Push(Packet(video, 2, 0, 2, true, 13), 1020000000, arrival);
  interleaver.Push(Packet(audio, 1024, 1024, 3, true, 17), 1021333333,
                   arrival + std::chrono::milliseconds(1));
  REQUIRE(interleaver.initialized());
  CHECK(interleaver.queued_packets() == 2);
  CHECK(interleaver.queued_bytes() == 30);
  const auto output = interleaver.PopReady(true);
  REQUIRE(output.size() == 2);
  CHECK(output.front().queued_at == arrival);
  CHECK(output.back().queued_at == arrival + std::chrono::milliseconds(1));
  CHECK(interleaver.queued_packets() == 0);
  CHECK(interleaver.queued_bytes() == 0);
  CHECK(interleaver.discarded_packets() == 2);
  CHECK(interleaver.discarded_bytes() == 18);
}

// Exercise this interleaver's startup selection across arrival order, track
// origins and frame rates. Verify selected first packets, retained counts,
// normalized media timestamps, DTS ordering and unchanged input packets.
TEST_CASE("交织器在不同到达顺序起点和帧率下正确选包并归一化媒体时间",
          "[remuxer][interleave][startup]") {
  struct Fixture {
    const char* name;
    int video_num;
    int video_den;
    int video_base;
    int audio_base;
    std::int64_t audio_shift_us;
    bool audio_first;
    int callback_delay;
    int first_video_id;
    int first_audio_id;
    std::size_t retained;
  };
  const Fixture fixtures[] = {
      {"视频包先到达", 1, 50, 0, -1, 0, false, 0, 0, 180, 370},
      {"音频包先到达", 1, 50, 0, -1, 0, true, 0, 0, 180, 370},
      {"音频回调延迟到达", 1, 50, 0, -1, 0, false, 8, 0, 180, 370},
      {"非零起点的两轨接近对齐", 1, 50, 250, 234, 0, false, 0, 0, 180, 370},
      {"音频提前到达时裁剪启动前缀", 1, 50, 250, 224, 0, true, 0, 0, 190, 360},
      {"音频起点较晚时重新选择起始包", 1, 50, 0, 4, 0, false, 0, 60, 232, 258},
      {"音频排序时钟延后时重新选择起始包", 1, 50, 0, -1, 100000, false, 0, 60,
       233, 257},
      {"音频排序时钟无额外偏移", 1, 50, 0, -1, 0, false, 0, 0, 180, 370},
      {"非整数帧率的时间基换算", 1001, 30000, 0, -1, 0, false, 0, 0, 180, 480}};
  for (const auto& fixture : fixtures) {
    DYNAMIC_SECTION(fixture.name) {
      const auto video = Video({fixture.video_den, fixture.video_num});
      const auto audio = Audio();
      PacketInterleaver interleaver({video, audio});
      struct Arrival {
        ffmpeg::Packet packet;
        std::int64_t clock;
      };
      std::vector<Arrival> videos;
      std::vector<Arrival> audios;
      constexpr std::int64_t origin_us = 10000000;
      for (int index = 0; index < 180; ++index) {
        const auto pts = fixture.video_base + index;
        const auto clock_us = origin_us + static_cast<std::int64_t>(pts) *
                                              fixture.video_num * 1000000 /
                                              fixture.video_den;
        videos.push_back({Packet(video, pts, pts - 2, index, index % 60 == 0),
                          clock_us * 1000});
      }
      const int audio_count = fixture.video_num == 1 ? 190 : 300;
      for (int index = 0; index < audio_count; ++index) {
        const auto pts =
            static_cast<std::int64_t>(fixture.audio_base + index) * 1024;
        const auto clock_us =
            origin_us + pts * 1000000 / 48000 + fixture.audio_shift_us;
        audios.push_back(
            {Packet(audio, pts, pts, 180 + index), clock_us * 1000});
      }
      std::vector<PacketInterleaver::Item> output;
      const auto push = [&](const Arrival& value) {
        interleaver.Push(value.packet, value.clock);
        Append(output, interleaver.PopReady());
      };
      if (fixture.audio_first) {
        for (const auto& value : audios) push(value);
        for (const auto& value : videos) push(value);
      } else {
        std::size_t next_audio = 0;
        for (std::size_t index = 0; index < videos.size(); ++index) {
          push(videos[index]);
          if (index >= static_cast<std::size_t>(fixture.callback_delay)) {
            for (int count = 0; count < 2 && next_audio < audios.size();
                 ++count) {
              push(audios[next_audio++]);
            }
          }
        }
        while (next_audio < audios.size()) push(audios[next_audio++]);
      }
      Append(output, interleaver.PopReady(true));
      REQUIRE(interleaver.initialized());
      REQUIRE(output.size() == fixture.retained);
      const auto first_video =
          std::find_if(output.begin(), output.end(), [&](const auto& value) {
            return value.packet->stream_index == video.stream_index;
          });
      const auto first_audio =
          std::find_if(output.begin(), output.end(), [&](const auto& value) {
            return value.packet->stream_index == audio.stream_index;
          });
      REQUIRE(first_video != output.end());
      REQUIRE(first_audio != output.end());
      CHECK(first_video->packet->pos == fixture.first_video_id);
      CHECK(first_audio->packet->pos == fixture.first_audio_id);
      CHECK((first_video->packet->flags & AV_PKT_FLAG_KEY) != 0);
      CHECK(first_video->pts_ns == 0);
      CHECK(first_video->dts_ns ==
            av_rescale_q(-2, video.time_base, encoder_test::kNanoseconds));
      CHECK(first_audio->pts_ns ==
            (first_audio->packet->pts < 0 ? -21333333 : 0));
      for (const auto& value : output) {
        const bool is_video = value.packet->stream_index == video.stream_index;
        const auto offset =
            is_video ? first_video->packet->pts
                     : std::max<std::int64_t>(first_audio->packet->dts, 0);
        CHECK(value.pts_ns == av_rescale_q(value.packet->pts - offset,
                                           value.packet->time_base,
                                           encoder_test::kNanoseconds));
        CHECK(value.dts_ns == av_rescale_q(value.packet->dts - offset,
                                           value.packet->time_base,
                                           encoder_test::kNanoseconds));
      }
      for (std::size_t index = 1; index < output.size(); ++index) {
        CHECK(output[index].dts_ns >= output[index - 1].dts_ns);
      }
      // A selected prefix changes only the side-channel media timestamps.
      CHECK(first_video->packet->pts ==
            fixture.video_base + fixture.first_video_id);
      CHECK(first_audio->packet->pts ==
            static_cast<std::int64_t>(fixture.audio_base +
                                      fixture.first_audio_id - 180) *
                1024);
    }
  }
}

TEST_CASE("首视频等待关键帧且音频覆盖起点后才初始化",
          "[remuxer][interleave][startup]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  interleaver.Push(Packet(audio, 0, 0), 1000000000);
  interleaver.Push(Packet(video, 1, 1, 1, false), 1010000000);
  CHECK_FALSE(interleaver.initialized());
  CHECK(interleaver.PopReady().empty());
  interleaver.Push(Packet(video, 2, 0, 2), 1020000000);
  CHECK_FALSE(interleaver.initialized());
  interleaver.Push(Packet(audio, 1024, 1024, 3), 1021333333);
  REQUIRE(interleaver.initialized());
  auto output = interleaver.PopReady(true);
  REQUIRE(output.size() == 2);
  CHECK(output.front().packet->pos == 2);
  CHECK(output.front().pts_ns == 0);
  CHECK(output.front().dts_ns == -40000000);
}

TEST_CASE("交织同DTS视频优先并等待对侧严格更晚媒体DTS",
          "[remuxer][interleave][ordering]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  interleaver.Push(Packet(audio, 0, 0, 1), 1000);
  interleaver.Push(Packet(video, 0, 0, 2), 1000);
  CHECK(interleaver.PopReady().empty());
  interleaver.Push(Packet(audio, 1024, 1024, 3), 21334333);
  auto first = interleaver.PopReady();
  REQUIRE(first.size() == 1);
  CHECK(first.front().packet->pos == 2);
  interleaver.Push(Packet(video, 1, 1, 4), 20001000);
  auto second = interleaver.PopReady();
  REQUIRE(second.size() == 2);
  CHECK(second[0].packet->pos == 1);
  CHECK(second[1].packet->pos == 4);
  auto tail = interleaver.PopReady(true);
  REQUIRE(tail.size() == 1);
  CHECK(tail[0].packet->pos == 3);
}

TEST_CASE("单轨直接放行并保留原包引用和时间基",
          "[remuxer][interleave][single]") {
  SECTION("视频") {
    const auto stream = Video();
    PacketInterleaver interleaver({stream});
    auto input = Packet(stream, 40, 38, 7);
    interleaver.Push(input, 5000000000);
    input.Unref();
    auto output = interleaver.PopReady();
    REQUIRE(output.size() == 1);
    CHECK(output[0].packet->pts == 40);
    CHECK(output[0].packet->dts == 38);
    CHECK(output[0].pts_ns == 0);
    CHECK(output[0].dts_ns == -40000000);
    CHECK(av_cmp_q(output[0].packet->time_base, stream.time_base) == 0);
  }
  SECTION("音频") {
    const auto stream = Audio();
    PacketInterleaver interleaver({stream});
    interleaver.Push(Packet(stream, -1024, -1024), 9000000000);
    auto output = interleaver.PopReady();
    REQUIRE(output.size() == 1);
    CHECK(output[0].pts_ns == -21333333);
    CHECK(output[0].dts_ns == -21333333);
  }
}

TEST_CASE("EOF缺轨或未覆盖起点仍排空有效轨且不补时间",
          "[remuxer][interleave][eof]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  SECTION("没有音频") {
    interleaver.Push(Packet(video, 50, 48), 1000000000);
    CHECK(interleaver.PopReady().empty());
    auto output = interleaver.PopReady(true);
    REQUIRE(output.size() == 1);
    CHECK(output[0].pts_ns == 0);
    CHECK(output[0].dts_ns == -40000000);
  }
  SECTION("没有视频") {
    interleaver.Push(Packet(audio, -1024, -1024), 1000000000);
    CHECK(interleaver.PopReady().empty());
    auto output = interleaver.PopReady(true);
    REQUIRE(output.size() == 1);
    CHECK(output[0].dts_ns == -21333333);
  }
  SECTION("音频结束在视频起点之前") {
    interleaver.Push(Packet(audio, -1024, -1024), 900000000);
    interleaver.Push(Packet(video, 0, -2), 1000000000);
    CHECK_FALSE(interleaver.initialized());
    auto output = interleaver.PopReady(true);
    REQUIRE(output.size() == 2);
    CHECK(output[0].dts_ns == -40000000);
    CHECK(output[1].dts_ns == -21333333);
  }
  SECTION("没有视频关键帧") {
    interleaver.Push(Packet(video, 0, 0, 0, false), 1000);
    interleaver.Push(Packet(audio, -1024, -1024), 2000);
    auto output = interleaver.PopReady(true);
    REQUIRE(output.size() == 1);
    CHECK(output[0].packet->stream_index == audio.stream_index);
  }
  SECTION("空会话") { CHECK(interleaver.PopReady(true).empty()); }
  CHECK(interleaver.PopReady(true).empty());
  CHECK_THROWS_AS(interleaver.Push(Packet(audio, 0, 0), 0), std::logic_error);
}

TEST_CASE("对侧轨道提前结束后的积压只在EOF排空", "[remuxer][interleave][eof]") {
  const auto video = Video();
  const auto audio = Audio();
  PacketInterleaver interleaver({video, audio});
  interleaver.Push(Packet(video, 0, 0), 1000);
  interleaver.Push(Packet(audio, 0, 0), 1000);
  std::vector<PacketInterleaver::Item> output;
  for (int index = 1; index <= 100; ++index) {
    interleaver.Push(Packet(video, index, index),
                     1000 + static_cast<std::int64_t>(index) * 20000000);
    Append(output, interleaver.PopReady());
  }
  CHECK(output.empty());
  Append(output, interleaver.PopReady(true));
  REQUIRE(output.size() == 102);
  for (std::size_t index = 1; index < output.size(); ++index) {
    CHECK(output[index].dts_ns >= output[index - 1].dts_ns);
  }
}

TEST_CASE("真实B帧和AAC交织排空保持两轨可解码及负延迟",
          "[remuxer][interleave][encoder]") {
  using namespace encoder_test;
  Capture capture;
  mw::streamer::Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  config.max_b_frames = 2;
  config.video_options["preset"] = "medium";
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  encoder.Start(config, {VideoStream(), AudioStream()}, cpu);
  REQUIRE(encoder.SubmitAudio(AudioFrame(40960)));
  for (int index = 0; index < 40; ++index) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(index)));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  PacketInterleaver interleaver(capture.streams);
  std::vector<PacketInterleaver::Item> output;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    interleaver.Push(capture.packets[index], capture.dts_ns[index]);
    Append(output, interleaver.PopReady());
  }
  Append(output, interleaver.PopReady(true));
  REQUIRE(output.size() == capture.packets.size());
  std::vector<ffmpeg::Packet> packets;
  for (const auto& value : output) packets.push_back(value.packet.Ref());
  for (const auto& stream : capture.streams) {
    const auto first =
        std::find_if(output.begin(), output.end(), [&](const auto& value) {
          return value.packet->stream_index == stream.stream_index;
        });
    REQUIRE(first != output.end());
    CHECK(first->dts_ns < 0);
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(first->pts_ns == 0);
      ffmpeg::VideoDecoder decoder(stream);
      CHECK(Decode(decoder, packets, stream.stream_index).size() == 40);
    } else {
      ffmpeg::AudioDecoder decoder(stream);
      CHECK_FALSE(Decode(decoder, packets, stream.stream_index).empty());
    }
  }
  for (std::size_t index = 1; index < output.size(); ++index) {
    CHECK(output[index].dts_ns >= output[index - 1].dts_ns);
  }
}

}  // namespace

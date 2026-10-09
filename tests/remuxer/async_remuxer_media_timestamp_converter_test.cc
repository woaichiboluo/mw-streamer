#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../encoder/encoder_test_support.h"
#include "mw/streamer/remuxer/media_timestamp_converter.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::MediaTimestampConverter;

ffmpeg::StreamInfo Video(AVRational time_base = {1, 25}) {
  auto stream = encoder_test::VideoStream();
  stream.time_base = time_base;
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

TEST_CASE("视频以首个PTS归零且保留负DTS和B帧展示次序",
          "[remuxer][async][timestamp][video]") {
  const auto stream = Video();
  MediaTimestampConverter converter({stream});
  auto first = Packet(stream, 100, 98);
  const auto timestamp = converter.Convert(first);
  CHECK(timestamp.pts_ns == 0);
  CHECK(timestamp.dts_ns == -80000000);
  CHECK(first->pts == 100);
  CHECK(first->dts == 98);
  CHECK(first->stream_index == stream.stream_index);
  CHECK(av_cmp_q(first->time_base, stream.time_base) == 0);

  const auto future = converter.Convert(Packet(stream, 103, 99));
  CHECK(future.pts_ns == 120000000);
  CHECK(future.dts_ns == -40000000);
  const auto reordered = converter.Convert(Packet(stream, 101, 100));
  CHECK(reordered.pts_ns == 40000000);
  CHECK(reordered.dts_ns == 0);
  CHECK(reordered.pts_ns < future.pts_ns);
  CHECK(reordered.dts_ns > future.dts_ns);
}

TEST_CASE("AAC保留负priming和零起点或平移正DTS入口",
          "[remuxer][async][timestamp][audio]") {
  const auto stream = Audio();
  MediaTimestampConverter converter({stream});
  SECTION("负priming不作为归零偏移") {
    constexpr std::int64_t expected[] = {-21333333, 0, 21333333, 42666667};
    for (int index = 0; index < 4; ++index) {
      const auto ticks = (index - 1) * 1024;
      const auto timestamp = converter.Convert(Packet(stream, ticks, ticks));
      CHECK(timestamp.pts_ns == expected[index]);
      CHECK(timestamp.dts_ns == expected[index]);
    }
  }
  SECTION("零DTS是有效入口") {
    const auto first = converter.Convert(Packet(stream, 0, 0));
    CHECK(first.pts_ns == 0);
    CHECK(first.dts_ns == 0);
    CHECK(converter.Convert(Packet(stream, 1024, 1024)).pts_ns == 21333333);
  }
  SECTION("正DTS入口同量平移PTS和DTS") {
    const auto first = converter.Convert(Packet(stream, 48128, 48000));
    CHECK(first.pts_ns == 2666667);
    CHECK(first.dts_ns == 0);
    const auto next = converter.Convert(Packet(stream, 49152, 49024));
    CHECK(next.pts_ns == 24000000);
    CHECK(next.dts_ns == 21333333);
  }
}

TEST_CASE("轨道独立初始化且接受等价完整时间基而不累计漂移",
          "[remuxer][async][timestamp][timebase]") {
  const auto video = Video({1001, 30000});
  const auto audio = Audio();
  MediaTimestampConverter converter({video, audio});
  CHECK(converter.Convert(Packet(audio, -1024, -1024)).pts_ns == -21333333);
  auto first = Packet(video, 900000, 899998);
  first->time_base = {2002, 60000};
  const auto start = converter.Convert(first);
  CHECK(start.pts_ns == 0);
  CHECK(start.dts_ns == -66733333);
  for (std::int64_t index = 1; index <= 10000; ++index) {
    const auto timestamp =
        converter.Convert(Packet(video, 900000 + index, 899998 + index));
    const auto expected_pts = (index * 1001000000000LL + 15000) / 30000;
    CHECK(timestamp.pts_ns == expected_pts);
  }
  const auto last = converter.Convert(Packet(video, 1900000, 1899998));
  CHECK(last.pts_ns == 33366666666667LL);
  CHECK(last.dts_ns == 33366599933333LL);
  CHECK(converter.Convert(Packet(audio, 0, 0)).pts_ns == 0);
}

TEST_CASE("Reset为新会话重新确定每条轨道的首包偏移",
          "[remuxer][async][timestamp][reset]") {
  const auto video = Video();
  const auto audio = Audio();
  MediaTimestampConverter converter({video, audio});
  converter.Convert(Packet(video, 100, 98));
  converter.Convert(Packet(audio, 48000, 48000));
  converter.Reset();
  converter.Reset();
  const auto new_video = converter.Convert(Packet(video, 400, 398));
  CHECK(new_video.pts_ns == 0);
  CHECK(new_video.dts_ns == -80000000);
  const auto new_audio = converter.Convert(Packet(audio, -1024, -1024));
  CHECK(new_audio.pts_ns == -21333333);
  CHECK(new_audio.dts_ns == -21333333);
}

TEST_CASE("拒绝非法轨道配置和重复索引",
          "[remuxer][async][timestamp][validation]") {
  auto stream = Video();
  SECTION("负索引") { stream.stream_index = -1; }
  SECTION("零时间基") { stream.time_base = {0, 25}; }
  SECTION("负时间基") { stream.time_base = {1, -25}; }
  SECTION("没有编码格式") {
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_NONE;
  }
  SECTION("非音视频轨道") {
    stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_DATA;
  }
  CHECK_THROWS_AS(MediaTimestampConverter({stream}), std::invalid_argument);
  auto duplicate = Audio();
  duplicate.stream_index = Video().stream_index;
  CHECK_THROWS_AS(MediaTimestampConverter({Video(), duplicate}),
                  std::invalid_argument);
}

TEST_CASE("非法包不建立或改变轨道偏移且原包保持不变",
          "[remuxer][async][timestamp][validation]") {
  const auto stream = Video();
  MediaTimestampConverter converter({stream});
  auto invalid = Packet(stream, 900, 898);
  SECTION("缺PTS") { invalid->pts = AV_NOPTS_VALUE; }
  SECTION("缺DTS") { invalid->dts = AV_NOPTS_VALUE; }
  SECTION("未知索引") { invalid->stream_index = 99; }
  SECTION("缺时间基") { invalid->time_base = {0, 1}; }
  SECTION("负时间基") { invalid->time_base = {-1, 25}; }
  SECTION("时间基不符") { invalid->time_base = {1, 50}; }
  SECTION("已移动空包") {
    auto retained = std::move(invalid);
    REQUIRE(retained.get() != nullptr);
    REQUIRE(invalid.get() == nullptr);
  }
  CHECK_THROWS_AS(converter.Convert(invalid), std::invalid_argument);
  const auto first = converter.Convert(Packet(stream, 100, 98));
  CHECK(first.pts_ns == 0);
  CHECK(first.dts_ns == -80000000);
  CHECK_THROWS_AS(converter.Convert(invalid), std::invalid_argument);
  const auto next = converter.Convert(Packet(stream, 101, 99));
  CHECK(next.pts_ns == 40000000);
  CHECK(next.dts_ns == -40000000);
}

TEST_CASE("平移或纳秒溢出不会确定失败首包的偏移",
          "[remuxer][async][timestamp][overflow]") {
  const auto stream = Video();
  MediaTimestampConverter converter({stream});
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  auto invalid = Packet(stream, 0, 0);
  SECTION("减去正偏移下溢") {
    invalid->pts = maximum;
    invalid->dts = minimum + 1;
  }
  SECTION("减去负偏移上溢") {
    invalid->pts = minimum + 1;
    invalid->dts = maximum;
  }
  SECTION("正DTS纳秒溢出") { invalid->dts = maximum; }
  SECTION("负DTS纳秒溢出") { invalid->dts = minimum + 1; }
  CHECK_THROWS_AS(converter.Convert(invalid), std::overflow_error);
  const auto first = converter.Convert(Packet(stream, 100, 98));
  CHECK(first.pts_ns == 0);
  CHECK(first.dts_ns == -80000000);
  CHECK_THROWS_AS(converter.Convert(Packet(stream, minimum + 1, 99)),
                  std::overflow_error);
  const auto next = converter.Convert(Packet(stream, 101, 99));
  CHECK(next.pts_ns == 40000000);
  CHECK(next.dts_ns == -40000000);
}

TEST_CASE("音频PTS纳秒溢出不改变首次正DTS归零",
          "[remuxer][async][timestamp][overflow]") {
  const auto stream = Audio();
  MediaTimestampConverter converter({stream});
  CHECK_THROWS_AS(converter.Convert(Packet(
                      stream, std::numeric_limits<std::int64_t>::max(), 0)),
                  std::overflow_error);
  const auto first = converter.Convert(Packet(stream, 48000, 48000));
  CHECK(first.pts_ns == 0);
  CHECK(first.dts_ns == 0);
}

TEST_CASE("真实B帧与AAC转换保持原生媒体时间并可解码",
          "[remuxer][async][timestamp][encoder][integration]") {
  using namespace encoder_test;
  constexpr int kFrames = 40;
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
  REQUIRE(encoder.SubmitAudio(AudioFrame(3072)));
  for (int index = 0; index < kFrames; ++index) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(index)));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.callback_error.empty());
  REQUIRE(capture.streams.size() == 2);

  MediaTimestampConverter converter(capture.streams);
  int videos = 0;
  int audios = 0;
  std::vector<std::int64_t> presentation;
  for (const auto& packet : capture.packets) {
    const auto raw_pts = packet->pts;
    const auto raw_dts = packet->dts;
    const auto timestamp = converter.Convert(packet);
    CHECK(packet->pts == raw_pts);
    CHECK(packet->dts == raw_dts);
    if (packet->stream_index == VideoStream().stream_index) {
      if (videos == 0) CHECK(timestamp.pts_ns == 0);
      CHECK(timestamp.dts_ns == (videos - 2) * kFrameInterval);
      presentation.push_back(timestamp.pts_ns);
      ++videos;
    } else {
      REQUIRE(packet->stream_index == AudioStream().stream_index);
      constexpr std::int64_t expected[] = {-21333333, 0, 21333333, 42666667};
      REQUIRE(audios < 4);
      CHECK(timestamp.pts_ns == expected[audios]);
      CHECK(timestamp.dts_ns == expected[audios]);
      ++audios;
    }
  }
  CHECK(videos == kFrames);
  CHECK(audios == 4);
  std::sort(presentation.begin(), presentation.end());
  for (int index = 0; index < videos; ++index) {
    CHECK(presentation[static_cast<std::size_t>(index)] ==
          index * kFrameInterval);
  }
  for (const auto& stream : capture.streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      ffmpeg::VideoDecoder decoder(stream);
      const auto frames = Decode(decoder, capture.packets, stream.stream_index);
      REQUIRE(frames.size() == kFrames);
      CHECK(frames.front()->pts == 0);
      CHECK(std::abs(static_cast<int>(frames.front()->data[0][0]) - 20) <= 2);
    } else {
      ffmpeg::AudioDecoder decoder(stream);
      const auto frames = Decode(decoder, capture.packets, stream.stream_index);
      bool checked_samples = false;
      for (const auto& frame : frames) {
        if (frame->pts != 0) continue;
        REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
        REQUIRE(frame->nb_samples > 256);
        const auto* samples =
            reinterpret_cast<const float*>(frame->extended_data[0]);
        double mean = 0;
        for (int index = 256; index < frame->nb_samples; ++index) {
          mean += static_cast<double>(samples[index]);
        }
        mean /= frame->nb_samples - 256;
        CHECK(std::abs(mean - 0.1) < 0.05);
        checked_samples = true;
      }
      CHECK(checked_samples);
    }
  }
}

}  // namespace

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "encoder_test_support.h"

namespace {

using namespace encoder_test;
using mw::streamer::Encoder;

TEST_CASE("满视频缓存重复最后画面并保留八个连续编码时刻",
          "[encoder][video][timing]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder, &gate);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream()}, cpu);
  ReleaseGate release{gate};
  auto first = VideoFrame(0);
  REQUIRE(encoder.SubmitVideo(first));
  REQUIRE(gate.Wait());
  for (int picture = 1; picture < 8; ++picture) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(picture)));
  }
  encoder.Drain();
  CHECK_FALSE(encoder.SubmitVideo(VideoFrame(8)));
  gate.Release();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.packets.size() == 8);
  REQUIRE(capture.streams.size() == 1);
  const auto& stream = capture.streams.front();
  for (int index = 0; index < 8; ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == stream.stream_index);
    CHECK(av_cmp_q(packet->time_base, stream.time_base) == 0);
    CHECK(av_rescale_q(packet->pts, packet->time_base, kNanoseconds) ==
          index * kFrameInterval);
    CHECK(capture.dts_ns[index] == kStart + index * kFrameInterval);
  }
  CHECK(first->pts == kStart);
  CHECK(first->duration == kFrameInterval);
  ffmpeg::VideoDecoder decoder(stream);
  const auto frames = Decode(decoder, capture.packets, stream.stream_index);
  REQUIRE(frames.size() == 8);
  constexpr int kPictures[] = {0, 1, 2, 3, 4, 5, 5, 5};
  for (int index = 0; index < 8; ++index) {
    REQUIRE(frames[index]->format == AV_PIX_FMT_YUV420P);
    for (int plane = 0; plane < 3; ++plane) {
      const int size = plane == 0 ? 64 : 32;
      const int expected = plane == 0 ? 20 + 20 * kPictures[index] : 128;
      int max_error = 0;
      for (int row = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column) {
          const int value =
              frames[index]
                  ->data[plane][row * frames[index]->linesize[plane] + column];
          max_error = std::max(max_error, std::abs(value - expected));
        }
      }
      CHECK(max_error <= 2);
    }
  }
}

TEST_CASE("纯音频按样本累计PTS并保留AAC编码延迟", "[encoder][audio][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {AudioStream()}, cpu);
  const auto input = AudioFrame(3072);
  REQUIRE(encoder.SubmitAudio(input));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  REQUIRE(capture.streams.size() == 1);
  const auto& stream = capture.streams.front();
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(stream.codec_parameters.get()->sample_rate == 48000);
  CHECK(stream.codec_parameters.get()->format == AV_SAMPLE_FMT_FLTP);
  CHECK(stream.codec_parameters.get()->ch_layout.nb_channels == 2);
  CHECK(av_cmp_q(stream.time_base, {1, 48000}) == 0);
  REQUIRE(capture.packets.size() == 4);
  for (int index = 0; index < 4; ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == stream.stream_index);
    CHECK(packet->pts == (index - 1) * 1024);
    CHECK(packet->dts == packet->pts);
    CHECK(capture.dts_ns[index] ==
          kStart + av_rescale_q(packet->dts, stream.time_base, kNanoseconds));
  }
  CHECK(input->pts == kStart);
  CHECK(input->nb_samples == 3072);
}

TEST_CASE("音频等待视频起点并裁剪提前二十毫秒的样本",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto audio = AudioFrame(3008, kStart - kFrameInterval, 960);
  REQUIRE(encoder.SubmitAudio(audio));
  REQUIRE(encoder.SubmitVideo(VideoFrame(0)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.streams.size() == 2);
  const ffmpeg::StreamInfo* audio_stream = nullptr;
  const ffmpeg::StreamInfo* video_stream = nullptr;
  for (const auto& stream : capture.streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream = &stream;
    } else {
      video_stream = &stream;
    }
  }
  REQUIRE(audio_stream != nullptr);
  REQUIRE(video_stream != nullptr);
  int audio_packets = 0;
  int video_packets = 0;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (packet->stream_index == audio_stream->stream_index) {
      CHECK(packet->pts == (audio_packets - 1) * 1024);
      CHECK(capture.dts_ns[index] ==
            kStart +
                av_rescale_q(packet->dts, packet->time_base, kNanoseconds));
      ++audio_packets;
    } else {
      CHECK(packet->stream_index == video_stream->stream_index);
      CHECK(packet->pts == 0);
      CHECK(capture.dts_ns[index] == kStart);
      ++video_packets;
    }
  }
  CHECK(audio_packets == 3);
  CHECK(video_packets == 1);
  ffmpeg::AudioDecoder decoder(*audio_stream);
  const auto frames =
      Decode(decoder, capture.packets, audio_stream->stream_index);
  bool checked_samples = false;
  for (const auto& frame : frames) {
    if (frame->pts != 0) continue;
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    double mean = 0;
    for (int index = 256; index < frame->nb_samples; ++index)
      mean += samples[index];
    mean /= frame->nb_samples - 256;
    CHECK(std::abs(mean - 0.1) < 0.05);
    checked_samples = true;
  }
  CHECK(checked_samples);
  CHECK(audio->pts == kStart - kFrameInterval);
  CHECK(audio->nb_samples == 3008);
}

TEST_CASE("零时间戳可以作为音视频会话的有效同步起点",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto video = VideoFrame(0);
  video->pts = 0;
  REQUIRE(encoder.SubmitVideo(video));
  REQUIRE(encoder.SubmitAudio(AudioFrame(2048, 0)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  int video_packets = 0;
  int audio_packets = 0;
  const int video_index = VideoStream().stream_index;
  const int audio_index = AudioStream().stream_index;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (packet->stream_index == video_index) {
      CHECK(packet->pts == 0);
      CHECK(capture.dts_ns[index] == 0);
      ++video_packets;
    } else {
      CHECK(packet->stream_index == audio_index);
      CHECK(packet->pts == (audio_packets - 1) * 1024);
      CHECK(capture.dts_ns[index] ==
            av_rescale_q(packet->dts, packet->time_base, kNanoseconds));
      ++audio_packets;
    }
  }
  CHECK(video_packets == 1);
  CHECK(audio_packets == 3);
}

TEST_CASE("B帧延迟编码排空并保持原生DTS与同步域映射",
          "[encoder][video][timing]") {
  constexpr int kFrames = 40;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  config.max_b_frames = 2;
  config.video_options["preset"] = "medium";
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  encoder.Start(config, {VideoStream()}, cpu);
  for (int index = 0; index < kFrames; ++index) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(index)));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.packets.size() == kFrames);
  REQUIRE(capture.streams.size() == 1);
  const auto first_dts = capture.packets.front()->dts;
  CHECK(first_dts < 0);
  CHECK(capture.dts_ns.front() == kStart);
  std::vector<std::int64_t> presentation_times;
  for (int index = 0; index < kFrames; ++index) {
    const auto& packet = capture.packets[index];
    if (index > 0) CHECK(packet->dts > capture.packets[index - 1]->dts);
    CHECK(capture.dts_ns[index] ==
          kStart + av_rescale_q(packet->dts - first_dts, packet->time_base,
                                kNanoseconds));
    presentation_times.push_back(
        av_rescale_q(packet->pts, packet->time_base, {1, 50}));
  }
  std::sort(presentation_times.begin(), presentation_times.end());
  for (int index = 0; index < kFrames; ++index) {
    CHECK(presentation_times[index] == index);
  }
  const auto& stream = capture.streams.front();
  ffmpeg::VideoDecoder decoder(stream);
  const auto frames = Decode(decoder, capture.packets, stream.stream_index);
  CHECK(frames.size() == kFrames);
}

}  // namespace

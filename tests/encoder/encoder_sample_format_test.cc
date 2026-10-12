#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "encoder_test_support.h"

namespace {

using namespace encoder_test;
using mw::streamer::Encoder;
constexpr int kRate = 48000;
constexpr int kSamples = 6400;
constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kSourceStart = 480000;

ffmpeg::StreamInfo S32Stream() {
  auto stream = AudioStream();
  stream.time_base = {1, kRate};
  stream.codec_parameters.get()->format = AV_SAMPLE_FMT_S32;
  return stream;
}

ffmpeg::Frame S32Frame(int offset, int samples) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_S32;
  frame->sample_rate = kRate;
  frame->nb_samples = samples;
  frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  frame->time_base = {1, kRate};
  frame->pts = kSourceStart + offset;
  frame->best_effort_timestamp = frame->pts;
  frame->pkt_dts = frame->pts - 16;
  frame->duration = samples;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 S32 编码测试帧");
  auto* data = reinterpret_cast<std::int32_t*>(frame->data[0]);
  for (int sample = 0; sample < samples; ++sample) {
    for (int channel = 0; channel < 2; ++channel) {
      const double frequency = channel == 0 ? 1500.0 : 3000.0;
      const double amplitude = channel == 0 ? 0.5 : 0.25;
      const double value =
          amplitude * std::sin(2.0 * kPi * frequency * (offset + sample) /
                               static_cast<double>(kRate));
      data[sample * 2 + channel] = static_cast<std::int32_t>(
          std::llround(value * std::numeric_limits<std::int32_t>::max()));
    }
  }
  return frame;
}

TEST_CASE("Encoder 将原始 S32 音频转换成 AAC 原生格式并保留源帧",
          "[encoder][audio][sample-format]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  const auto source = S32Stream();
  encoder.Start(Config(), {source}, cpu);
  REQUIRE(capture.streams.size() == 1);
  const auto& output = capture.streams.front();
  CHECK(source.codec_parameters.get()->format == AV_SAMPLE_FMT_S32);
  CHECK(output.codec_parameters.get()->format == AV_SAMPLE_FMT_FLTP);
  CHECK(output.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(output.codec_parameters.get()->sample_rate == kRate);
  CHECK(output.codec_parameters.get()->ch_layout.nb_channels == 2);
  std::vector<ffmpeg::Frame> retained;
  std::vector<std::vector<std::int32_t>> original_samples;
  int offset = 0;
  for (const int samples : {kSamples}) {
    auto frame = S32Frame(offset, samples);
    retained.push_back(frame.Ref());
    const auto* data = reinterpret_cast<const std::int32_t*>(frame->data[0]);
    original_samples.emplace_back(data, data + samples * 2);
    REQUIRE(encoder.SubmitFrame(Audio(frame)));
    offset += samples;
  }
  REQUIRE(offset == kSamples);
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  CHECK(capture.ended == 1);
  CHECK(capture.callback_error.empty());
  offset = 0;
  for (std::size_t index = 0; index < retained.size(); ++index) {
    const auto& frame = retained[index];
    CHECK(frame->format == AV_SAMPLE_FMT_S32);
    CHECK(frame->sample_rate == kRate);
    CHECK(frame->pts == kSourceStart + offset);
    CHECK(frame->best_effort_timestamp == frame->pts);
    CHECK(frame->pkt_dts == frame->pts - 16);
    CHECK(frame->duration == frame->nb_samples);
    CHECK(av_cmp_q(frame->time_base, AVRational{1, kRate}) == 0);
    CHECK(std::memcmp(frame->data[0], original_samples[index].data(),
                      original_samples[index].size() * sizeof(std::int32_t)) ==
          0);
    offset += frame->nb_samples;
  }
  REQUIRE_FALSE(capture.packets.empty());
  std::int64_t encoded_samples = 0;
  for (const auto& packet : capture.packets) {
    REQUIRE(packet->duration > 0);
    encoded_samples +=
        av_rescale_q(packet->duration, output.time_base, {1, kRate});
  }
  CHECK(encoded_samples - output.codec_parameters.get()->initial_padding ==
        kSamples);
  CHECK(av_rescale_q(capture.packets.back()->duration, output.time_base,
                     {1, kRate}) == 256);
  ffmpeg::AudioDecoder decoder(output);
  const auto frames = Decode(decoder, capture.packets, output.stream_index);
  REQUIRE_FALSE(frames.empty());
  double sine[2][2]{};
  double cosine[2][2]{};
  int analyzed = 0;
  // Skip codec startup and tail transients. Both frequencies complete an exact
  // number of cycles in this window, so the projection also detects crosstalk.
  for (const auto& frame : frames) {
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    CHECK(frame->sample_rate == kRate);
    REQUIRE(frame->ch_layout.nb_channels == 2);
    REQUIRE(frame->pts != AV_NOPTS_VALUE);
    const auto first = av_rescale_q(frame->pts, output.time_base, {1, kRate});
    for (int sample = 0; sample < frame->nb_samples; ++sample) {
      const auto position = first + sample;
      if (position < 1024 || position >= kSamples - 1024) continue;
      ++analyzed;
      for (int channel = 0; channel < 2; ++channel) {
        const auto* data =
            reinterpret_cast<const float*>(frame->extended_data[channel]);
        for (int frequency = 0; frequency < 2; ++frequency) {
          const double hz = frequency == 0 ? 1500.0 : 3000.0;
          const double phase =
              2.0 * kPi * hz * static_cast<double>(position) / kRate;
          sine[channel][frequency] +=
              static_cast<double>(data[sample]) * std::sin(phase);
          cosine[channel][frequency] +=
              static_cast<double>(data[sample]) * std::cos(phase);
        }
      }
    }
  }
  REQUIRE(analyzed == kSamples - 2048);
  const auto amplitude = [&](int channel, int frequency) {
    return 2.0 *
           std::hypot(sine[channel][frequency], cosine[channel][frequency]) /
           analyzed;
  };
  CHECK(std::abs(amplitude(0, 0) - 0.5) < 0.08);
  CHECK(std::abs(amplitude(1, 1) - 0.25) < 0.08);
  CHECK(amplitude(0, 1) < 0.04);
  CHECK(amplitude(1, 0) < 0.04);
}

TEST_CASE("Encoder 音频格式转换仍拒绝中途改变输入布局",
          "[encoder][audio][sample-format][validation]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {S32Stream()}, cpu);
  auto malformed = S32Frame(0, 800);
  SECTION("采样格式") { malformed->format = AV_SAMPLE_FMT_FLTP; }
  SECTION("采样率") { malformed->sample_rate = 44100; }
  SECTION("声道布局") {
    av_channel_layout_uninit(&malformed->ch_layout);
    malformed->ch_layout = AV_CHANNEL_LAYOUT_MONO;
  }
  CHECK_THROWS_AS(encoder.SubmitFrame(Audio(malformed)), std::invalid_argument);
  REQUIRE(encoder.SubmitFrame(Audio(S32Frame(0, 1600))));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  CHECK(capture.ended == 1);
  CHECK(capture.callback_error.empty());
  CHECK_FALSE(capture.packets.empty());
}

}  // namespace

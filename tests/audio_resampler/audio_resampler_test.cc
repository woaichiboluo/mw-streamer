#include "mw/streamer/filter/audio_resampler.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace {

using mw::streamer::AudioResampler;
using mw::streamer::AudioResamplerConfig;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
namespace ffmpeg = mw::streamer::ffmpeg;

AvDecoderReadResult Audio(int samples, std::int64_t pts, int rate = 48000,
                          AVSampleFormat format = AV_SAMPLE_FMT_FLTP,
                          int channels = 2, std::uint64_t generation = 1,
                          int stream_index = 7) {
  ffmpeg::Frame frame;
  frame->format = format;
  frame->sample_rate = rate;
  frame->nb_samples = samples;
  av_channel_layout_default(&frame->ch_layout, channels);
  frame->pts = pts;
  frame->time_base = {1, rate};
  frame->duration = samples;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("Cannot allocate audio");
  }
  for (int sample = 0; sample < samples; ++sample) {
    for (int channel = 0; channel < channels; ++channel) {
      const float value = static_cast<float>(
          std::sin(static_cast<double>(pts + sample) * 0.017 + channel) * 0.4);
      if (format == AV_SAMPLE_FMT_FLTP) {
        reinterpret_cast<float*>(frame->extended_data[channel])[sample] = value;
      } else if (format == AV_SAMPLE_FMT_S16) {
        reinterpret_cast<std::int16_t*>(
            frame->extended_data[0])[sample * channels + channel] =
            static_cast<std::int16_t>(value * 30000);
      } else {
        throw std::runtime_error("Unsupported test audio format");
      }
    }
  }
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.generation = generation;
  result.stream_index = stream_index;
  result.time_base = {1, rate};
  return result;
}

AvDecoderReadResult Eof(std::int64_t end_pts, int rate = 48000,
                        std::uint64_t generation = 1, int stream_index = 7) {
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kEof;
  result.end_pts = end_pts;
  result.time_base = {1, rate};
  result.generation = generation;
  result.stream_index = stream_index;
  return result;
}

struct Capture {
  std::vector<float> pcm;
  std::vector<std::int64_t> pts;
  std::vector<int> samples;
  std::vector<std::uint64_t> generations;
  std::vector<int> streams;
  std::vector<AvDecoderReadResult> eof;
};

void CaptureFrame(const AVFrame& frame, Capture& capture) {
  REQUIRE(frame.format == AV_SAMPLE_FMT_FLTP);
  capture.pts.push_back(frame.pts);
  capture.samples.push_back(frame.nb_samples);
  for (int sample = 0; sample < frame.nb_samples; ++sample) {
    for (int channel = 0; channel < frame.ch_layout.nb_channels; ++channel) {
      capture.pcm.push_back(
          reinterpret_cast<const float*>(frame.extended_data[channel])[sample]);
    }
  }
}

void Drain(AudioResampler& resampler, Capture& capture, int rate = 48000) {
  for (int iteration = 0; iteration < 10000; ++iteration) {
    auto result = resampler.TryReadFrame();
    if (result.status == AvDecoderReadStatus::kNull) return;
    if (result.status == AvDecoderReadStatus::kEof) {
      capture.eof.push_back(std::move(result));
      continue;
    }
    REQUIRE(result.frame.has_value());
    const AVFrame& frame = *result.frame->get();
    REQUIRE(result.time_base.num == 1);
    REQUIRE(result.time_base.den == rate);
    REQUIRE(frame.sample_rate == rate);
    REQUIRE(frame.time_base.num == 1);
    REQUIRE(frame.time_base.den == rate);
    REQUIRE(frame.nb_samples > 0);
    REQUIRE(frame.duration == frame.nb_samples);
    REQUIRE(frame.best_effort_timestamp == frame.pts);
    REQUIRE(frame.pkt_dts == AV_NOPTS_VALUE);
    capture.generations.push_back(result.generation);
    capture.streams.push_back(result.stream_index);
    CaptureFrame(frame, capture);
  }
  FAIL("AudioResampler never reached an empty queue");
}

// Independent native Swr provides a PCM and timestamp oracle. No timing repair,
// async compensation, frame-size reblocking, or silence insertion is enabled.
class NativeSwr {
 public:
  NativeSwr(const AVFrame& input, const AudioResamplerConfig& config)
      : input_rate_(input.sample_rate), output_rate_(config.sample_rate) {
    AVChannelLayout output_layout{};
    REQUIRE(av_channel_layout_copy(&output_layout, &config.channel_layout) ==
            0);
    AVChannelLayout input_layout{};
    REQUIRE(av_channel_layout_copy(&input_layout, &input.ch_layout) == 0);
    const int result = swr_alloc_set_opts2(
        &context_, &output_layout, config.sample_format, output_rate_,
        &input_layout, static_cast<AVSampleFormat>(input.format), input_rate_,
        0, nullptr);
    av_channel_layout_uninit(&input_layout);
    av_channel_layout_uninit(&output_layout);
    REQUIRE(result == 0);
    REQUIRE(swr_init(context_) == 0);
    av_channel_layout_copy(&layout_, &config.channel_layout);
  }
  ~NativeSwr() {
    swr_free(&context_);
    av_channel_layout_uninit(&layout_);
  }
  void Convert(const AvDecoderReadResult* input, Capture& output) {
    const AVFrame* frame = input ? input->frame->get() : nullptr;
    if (frame) {
      const auto source_pts =
          av_rescale_q(frame->pts, input->time_base, {1, input_rate_}) *
          output_rate_;
      next_pts_ = av_rescale_q(swr_next_pts(context_, source_pts),
                               {1, input_rate_}, {1, 1});
    }
    ffmpeg::Frame converted;
    converted->format = AV_SAMPLE_FMT_FLTP;
    converted->sample_rate = output_rate_;
    av_channel_layout_copy(&converted->ch_layout, &layout_);
    converted->nb_samples = std::max(
        1, swr_get_out_samples(context_, frame ? frame->nb_samples : 0));
    REQUIRE(av_frame_get_buffer(converted.get(), 0) == 0);
    const int count = swr_convert(
        context_, converted->extended_data, converted->nb_samples,
        frame ? const_cast<const std::uint8_t**>(frame->extended_data)
              : nullptr,
        frame ? frame->nb_samples : 0);
    REQUIRE(count >= 0);
    if (count == 0) return;
    converted->nb_samples = count;
    converted->pts = next_pts_;
    next_pts_ += count;
    CaptureFrame(*converted.get(), output);
  }
  void Flush(Capture& output) {
    for (;;) {
      const auto size = output.pcm.size();
      Convert(nullptr, output);
      if (output.pcm.size() == size) return;
    }
  }

 private:
  SwrContext* context_ = nullptr;
  AVChannelLayout layout_{};
  int input_rate_;
  int output_rate_;
  std::int64_t next_pts_ = 0;
};

void CheckPcm(const Capture& actual, const Capture& expected) {
  REQUIRE(actual.pcm.size() == expected.pcm.size());
  double maximum_error = 0;
  for (std::size_t index = 0; index < actual.pcm.size(); ++index) {
    maximum_error = std::max(
        maximum_error,
        static_cast<double>(std::abs(actual.pcm[index] - expected.pcm[index])));
  }
  REQUIRE(maximum_error < 1e-6);
}

TEST_CASE("AudioResampler preserves same-format samples and source timestamps",
          "[audio-resampler]") {
  AudioResampler resampler;
  Capture actual;
  Capture expected;
  for (int index = 0; index < 12; ++index) {
    auto input = Audio(257, 48000 + index * 257);
    CaptureFrame(*input.frame->get(), expected);
    REQUIRE(resampler.TryPushFrame(input));
    Drain(resampler, actual);
  }
  REQUIRE(resampler.TryPushFrame(Eof(48000 + 12 * 257)));
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts == 48000 + 12 * 257);
}

TEST_CASE("AudioResampler rate conversion matches native Swr including tails",
          "[audio-resampler]") {
  const int input_rate = GENERATE(44100, 48000);
  AudioResamplerConfig config;
  config.sample_rate = input_rate == 44100 ? 48000 : 44100;
  AudioResampler resampler(config);
  auto first = Audio(997, input_rate * 3, input_rate);
  NativeSwr oracle(*first.frame->get(), config);
  Capture expected;
  Capture actual;
  std::int64_t endpoint = input_rate * 3;
  for (int index = 0; index < 180; ++index) {
    auto input = Audio(997, endpoint, input_rate);
    oracle.Convert(&input, expected);
    REQUIRE(resampler.TryPushFrame(input));
    Drain(resampler, actual, config.sample_rate);
    endpoint += 997;
  }
  oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(Eof(endpoint, input_rate)));
  Drain(resampler, actual, config.sample_rate);
  CheckPcm(actual, expected);
  REQUIRE(actual.samples == expected.samples);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts ==
          av_rescale_q(endpoint, {1, input_rate}, {1, config.sample_rate}));
}

TEST_CASE("AudioResampler converts packed samples and remixes channels",
          "[audio-resampler]") {
  AudioResamplerConfig config;
  config.channel_layout = AV_CHANNEL_LAYOUT_MONO;
  AudioResampler resampler(config);
  auto input = Audio(2048, 96000, 48000, AV_SAMPLE_FMT_S16);
  NativeSwr oracle(*input.frame->get(), config);
  Capture expected;
  Capture actual;
  oracle.Convert(&input, expected);
  oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(input));
  REQUIRE(resampler.TryPushFrame(Eof(98048)));
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pcm.size() == 2048);
}

TEST_CASE(
    "AudioResampler buffers small inputs and drains remaining EOF samples",
    "[audio-resampler]") {
  AudioResampler resampler;
  auto first = Audio(3, 0, 44100);
  NativeSwr oracle(*first.frame->get(), AudioResamplerConfig{});
  Capture expected;
  Capture actual;
  REQUIRE(resampler.TryPushFrame(first));
  oracle.Convert(&first, expected);
  REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
  for (int index = 1; index < 40; ++index) {
    auto input = Audio(3, index * 3, 44100);
    oracle.Convert(&input, expected);
    REQUIRE(resampler.TryPushFrame(input));
    Drain(resampler, actual);
  }
  const auto before = actual.pcm.size();
  oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(Eof(120, 44100)));
  Drain(resampler, actual);
  REQUIRE(actual.pcm.size() > before);
  REQUIRE(actual.eof.size() == 1);
  CheckPcm(actual, expected);
}

TEST_CASE(
    "AudioResampler silent conversions retain the latest PTS anchor for EOF",
    "[audio-resampler]") {
  AudioResampler resampler;
  auto first = Audio(24, 0, 44100);
  auto jumped = Audio(1, 30 * 44100, 44100);
  NativeSwr oracle(*first.frame->get(), AudioResamplerConfig{});
  Capture expected;
  Capture actual;
  oracle.Convert(&first, expected);
  REQUIRE(expected.pcm.empty());
  REQUIRE(resampler.TryPushFrame(first));
  REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
  oracle.Convert(&jumped, expected);
  REQUIRE(expected.pcm.empty());
  REQUIRE(resampler.TryPushFrame(jumped));
  REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
  oracle.Flush(expected);
  REQUIRE_FALSE(expected.pcm.empty());
  REQUIRE(resampler.TryPushFrame(Eof(30 * 44100 + 1, 44100)));
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.pts.front() > 29 * 48000);
  REQUIRE(actual.eof.size() == 1);
}

TEST_CASE("AudioResampler drains old generations before admitting new timing",
          "[audio-resampler]") {
  AudioResampler resampler;
  auto old_input = Audio(100, 44100, 44100);
  auto new_input = Audio(100, 48000 * 31, 48000, AV_SAMPLE_FMT_S16, 1, 2, 9);
  NativeSwr old_oracle(*old_input.frame->get(), AudioResamplerConfig{});
  NativeSwr new_oracle(*new_input.frame->get(), AudioResamplerConfig{});
  Capture expected;
  old_oracle.Convert(&old_input, expected);
  old_oracle.Flush(expected);
  const auto old_frames = expected.pts.size();
  new_oracle.Convert(&new_input, expected);
  new_oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(old_input));
  REQUIRE(resampler.TryPushFrame(new_input));
  REQUIRE(resampler.TryPushFrame(Eof(48000 * 31 + 123, 48000, 2, 9)));
  Capture actual;
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.generations.size() == expected.pts.size());
  for (std::size_t index = 0; index < actual.generations.size(); ++index) {
    REQUIRE(actual.generations[index] == (index < old_frames ? 1 : 2));
    REQUIRE(actual.streams[index] == (index < old_frames ? 7 : 9));
  }
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts == 48000 * 31 + 123);
  REQUIRE(actual.eof.front().generation == 2);
}

TEST_CASE("AudioResampler drains changed source format in the same generation",
          "[audio-resampler]") {
  AudioResampler resampler;
  auto old_input = Audio(100, 0, 44100);
  auto new_input = Audio(113, 48000, 48000, AV_SAMPLE_FMT_S16, 1);
  NativeSwr old_oracle(*old_input.frame->get(), AudioResamplerConfig{});
  NativeSwr new_oracle(*new_input.frame->get(), AudioResamplerConfig{});
  Capture expected;
  old_oracle.Convert(&old_input, expected);
  old_oracle.Flush(expected);
  new_oracle.Convert(&new_input, expected);
  new_oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(old_input));
  REQUIRE(resampler.TryPushFrame(new_input));
  REQUIRE(resampler.TryPushFrame(Eof(48113)));
  Capture actual;
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
}

TEST_CASE("AudioResampler preserves jumps and jitter instead of synchronizing",
          "[audio-resampler]") {
  AudioResampler resampler;
  auto first = Audio(1024, 44100, 44100);
  NativeSwr oracle(*first.frame->get(), AudioResamplerConfig{});
  Capture expected;
  Capture actual;
  const std::vector<std::int64_t> timestamps = {
      44100,       45124,
      46149,       47172,
      44100 * 31,  44100 * 31 + 1024,
      44100 * -29, 44100 * -29 + 1024};
  for (auto pts : timestamps) {
    auto input = Audio(1024, pts, 44100);
    oracle.Convert(&input, expected);
    REQUIRE(resampler.TryPushFrame(input));
    Drain(resampler, actual);
  }
  oracle.Flush(expected);
  REQUIRE(resampler.TryPushFrame(Eof(timestamps.back() + 1024, 44100)));
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.pts[4] - actual.pts[3] > 48000 * 29);
  REQUIRE(actual.pts[6] - actual.pts[5] < -48000 * 59);
}

TEST_CASE("AudioResampler respects the source millisecond time base",
          "[audio-resampler]") {
  AudioResampler resampler;
  auto first = Audio(1024, 0, 44100);
  NativeSwr oracle(*first.frame->get(), AudioResamplerConfig{});
  Capture actual;
  Capture expected;
  for (const auto pts : {1001, 1024, 1047, 1071}) {
    auto input = Audio(1024, 0, 44100);
    (*input.frame)->pts = pts;
    (*input.frame)->time_base = {1, 1000};
    input.time_base = {1, 1000};
    oracle.Convert(&input, expected);
    REQUIRE(resampler.TryPushFrame(input));
    Drain(resampler, actual);
  }
  oracle.Flush(expected);
  auto eof = Eof(1094);
  eof.time_base = {1, 1000};
  REQUIRE(resampler.TryPushFrame(eof));
  Drain(resampler, actual);
  CheckPcm(actual, expected);
  REQUIRE(actual.pts == expected.pts);
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts == 1094 * 48);
}

TEST_CASE("AudioResampler EOF endpoint is not replaced by converted frame end",
          "[audio-resampler]") {
  AudioResampler resampler;
  REQUIRE(resampler.TryPushFrame(Audio(100, 44100, 44100)));
  REQUIRE(resampler.TryPushFrame(Eof(88200, 44100)));
  Capture actual;
  Drain(resampler, actual);
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts == 96000);
  REQUIRE(actual.pts.back() + actual.samples.back() < 96000);
}

TEST_CASE(
    "AudioResampler full queue rejects without consuming or changing order",
    "[audio-resampler]") {
  AudioResamplerConfig config;
  config.input_capacity = 1;
  AudioResampler resampler(config);
  auto first = Audio(128, 0);
  auto pending = Audio(128, 128);
  REQUIRE(resampler.TryPushFrame(first));
  REQUIRE_FALSE(resampler.TryPushFrame(pending));
  REQUIRE((*pending.frame)->pts == 128);
  Capture actual;
  Drain(resampler, actual);
  REQUIRE(actual.pts == std::vector<std::int64_t>{0});
  REQUIRE(resampler.TryPushFrame(pending));
  Drain(resampler, actual);
  REQUIRE(actual.pts == std::vector<std::int64_t>{0, 128});
  REQUIRE(actual.pcm.size() == 256 * 2);
}

TEST_CASE(
    "AudioResampler rejected EOF or generation cannot change admission state",
    "[audio-resampler]") {
  AudioResamplerConfig config;
  config.input_capacity = 1;
  AudioResampler resampler(config);
  REQUIRE(resampler.TryPushFrame(Audio(128, 0)));
  REQUIRE_FALSE(resampler.TryPushFrame(Eof(128)));
  REQUIRE_FALSE(resampler.TryPushFrame(
      Audio(128, 96000, 48000, AV_SAMPLE_FMT_FLTP, 2, 2)));
  Capture actual;
  Drain(resampler, actual);
  // Both rejected submissions leave the older generation open for more audio.
  REQUIRE(resampler.TryPushFrame(Audio(128, 128)));
  Drain(resampler, actual);
  REQUIRE(resampler.TryPushFrame(Eof(256)));
  Drain(resampler, actual);
  REQUIRE(actual.eof.size() == 1);
  REQUIRE(actual.eof.front().end_pts == 256);
  REQUIRE(actual.pts == std::vector<std::int64_t>{0, 128});
  resampler.Stop();
  REQUIRE_FALSE(resampler.TryPushFrame(Audio(128, 1000)));
  REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
  resampler.Reset();
  REQUIRE(resampler.TryPushFrame(Audio(128, 1000)));
  Capture restarted;
  Drain(resampler, restarted);
  REQUIRE(restarted.pts == std::vector<std::int64_t>{1000});
}

TEST_CASE("AudioResampler invalid submissions do not poison valid processing",
          "[audio-resampler]") {
  AudioResamplerConfig invalid;
  invalid.input_capacity = 0;
  REQUIRE_THROWS_AS(AudioResampler{invalid}, std::invalid_argument);
  invalid = AudioResamplerConfig{};
  invalid.sample_rate = 0;
  REQUIRE_THROWS_AS(AudioResampler{invalid}, std::invalid_argument);
  invalid = AudioResamplerConfig{};
  invalid.sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE_THROWS_AS(AudioResampler{invalid}, std::invalid_argument);
  AudioResampler resampler;
  auto input = Audio(128, 0);
  (*input.frame)->pts = AV_NOPTS_VALUE;
  REQUIRE_THROWS_AS(resampler.TryPushFrame(input), std::invalid_argument);
  REQUIRE(resampler.TryPushFrame(Audio(128, 48000)));
  REQUIRE(resampler.TryPushFrame(Eof(48128)));
  REQUIRE_THROWS_AS(resampler.TryPushFrame(Audio(128, 48128)),
                    std::invalid_argument);
  Capture actual;
  Drain(resampler, actual);
  REQUIRE(actual.pts.front() == 48000);
  REQUIRE(actual.eof.size() == 1);
}

TEST_CASE("AudioResampler owns custom target layouts independently",
          "[audio-resampler]") {
  AudioResamplerConfig config;
  REQUIRE(av_channel_layout_custom_init(&config.channel_layout, 2) == 0);
  config.channel_layout.u.map[0].id = AV_CHAN_FRONT_LEFT;
  config.channel_layout.u.map[1].id = AV_CHAN_FRONT_RIGHT;
  AudioResampler resampler(config);
  av_channel_layout_uninit(&config.channel_layout);
  REQUIRE(resampler.TryPushFrame(Audio(128, 1234)));
  auto result = resampler.TryReadFrame();
  REQUIRE(result.status == AvDecoderReadStatus::kFrame);
  REQUIRE((*result.frame)->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM);
  REQUIRE((*result.frame)->ch_layout.nb_channels == 2);
  REQUIRE((*result.frame)->ch_layout.u.map[0].id == AV_CHAN_FRONT_LEFT);
  REQUIRE((*result.frame)->ch_layout.u.map[1].id == AV_CHAN_FRONT_RIGHT);
}

}  // namespace

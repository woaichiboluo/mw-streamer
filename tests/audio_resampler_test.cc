#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/codec_parameters.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/resampler/audio_resampler.h"

namespace {

using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::resampler::AudioResampler;

StreamInfo MakeAudioStream(int sample_rate, int channel_count) {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters.get()->codec_id = AV_CODEC_ID_PCM_S16LE;
  parameters.get()->sample_rate = sample_rate;
  av_channel_layout_default(&parameters.get()->ch_layout, channel_count);
  return StreamInfo{0, std::move(parameters), AVRational{1, sample_rate}};
}

Frame MakeS16PlanarStereoFrame(int sample_count, std::int64_t pts,
                               int sample_rate) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_S16P;
  frame->sample_rate = sample_rate;
  frame->nb_samples = sample_count;
  frame->pts = pts;
  frame->duration = sample_count;
  frame->time_base = AVRational{1, sample_rate};
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配测试音频帧失败");
  }

  auto* left = reinterpret_cast<std::int16_t*>(frame->extended_data[0]);
  auto* right = reinterpret_cast<std::int16_t*>(frame->extended_data[1]);
  for (int index = 0; index < sample_count; ++index) {
    left[index] = 8192;
    right[index] = -16384;
  }
  return frame;
}

Frame MakeFloatPlanarMonoFrame(int sample_count, std::int64_t pts,
                               int sample_rate) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = sample_rate;
  frame->nb_samples = sample_count;
  frame->pts = pts;
  frame->duration = sample_count;
  frame->time_base = AVRational{1, sample_rate};
  av_channel_layout_default(&frame->ch_layout, 1);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配测试音频帧失败");
  }

  auto* samples = reinterpret_cast<float*>(frame->extended_data[0]);
  for (int index = 0; index < sample_count; ++index) {
    samples[index] = 0.5F;
  }
  return frame;
}

}  // namespace

TEST_CASE(
    "audio resampler converts planar S16 44.1 kHz to interleaved float32 "
    "48 kHz") {
  AudioResampler resampler(MakeAudioStream(44100, 2));

  std::int64_t expected_pts = 0;
  std::int64_t output_samples = 0;
  double left_sum = 0.0;
  double right_sum = 0.0;
  bool valid_output = true;
  resampler.SetOnFrame([&](const Frame& output) {
    if (output->format != AV_SAMPLE_FMT_FLT ||
        output->sample_rate != AudioResampler::kOutputSampleRate ||
        output->ch_layout.nb_channels != 2 || output->nb_samples <= 0 ||
        !output->data[0] || output->pts != expected_pts ||
        output->duration != output->nb_samples || output->time_base.num != 1 ||
        output->time_base.den != AudioResampler::kOutputSampleRate) {
      valid_output = false;
      return;
    }

    const auto* samples = reinterpret_cast<const float*>(output->data[0]);
    for (int index = 0; index < output->nb_samples; ++index) {
      const auto left = samples[index * 2];
      const auto right = samples[index * 2 + 1];
      if (!std::isfinite(left) || !std::isfinite(right)) {
        valid_output = false;
        return;
      }
      left_sum += left;
      right_sum += right;
    }
    expected_pts += output->nb_samples;
    output_samples += output->nb_samples;
  });

  constexpr int kInputFrameSamples = 441;
  constexpr int kInputFrameCount = 10;
  for (int index = 0; index < kInputFrameCount; ++index) {
    resampler.Resample(MakeS16PlanarStereoFrame(
        kInputFrameSamples, index * kInputFrameSamples, 44100));
  }
  resampler.Drain();

  CHECK(valid_output);
  CHECK(output_samples == 4800);
  CHECK(left_sum / static_cast<double>(output_samples) ==
        Catch::Approx(0.25).margin(0.01));
  CHECK(right_sum / static_cast<double>(output_samples) ==
        Catch::Approx(-0.5).margin(0.01));
}

TEST_CASE("audio resampler drain and flush reuse one instance") {
  AudioResampler resampler(MakeAudioStream(48000, 1));
  auto input = MakeFloatPlanarMonoFrame(64, 96000, 48000);

  std::vector<std::int64_t> output_pts;
  std::size_t output_samples = 0;
  resampler.SetOnFrame([&](const Frame& output) {
    output_pts.push_back(output->pts);
    output_samples += output->nb_samples;
  });

  resampler.Resample(input);
  resampler.Drain();
  CHECK(output_samples == 64);
  REQUIRE(output_pts.size() == 1);
  CHECK(output_pts.front() == 96000);
  CHECK_THROWS_AS(resampler.Resample(input), std::logic_error);
  CHECK_NOTHROW(resampler.Drain());

  resampler.Flush();
  resampler.Resample(input);
  resampler.Drain();
  CHECK(output_samples == 128);
  REQUIRE(output_pts.size() == 2);
  CHECK(output_pts.back() == 96000);
}

TEST_CASE("audio resampler flush discards delayed samples from old timeline") {
  AudioResampler resampler(MakeAudioStream(44100, 2));

  std::int64_t first_output_pts = AV_NOPTS_VALUE;
  std::int64_t output_samples = 0;
  resampler.SetOnFrame([&](const Frame& output) {
    if (first_output_pts == AV_NOPTS_VALUE) {
      first_output_pts = output->pts;
    }
    output_samples += output->nb_samples;
  });

  resampler.Resample(MakeS16PlanarStereoFrame(10, 0, 44100));
  resampler.Flush();
  first_output_pts = AV_NOPTS_VALUE;
  output_samples = 0;

  resampler.Resample(MakeS16PlanarStereoFrame(441, 44100, 44100));
  resampler.Drain();

  CHECK(first_output_pts == 48000);
  CHECK(output_samples == 480);
}

TEST_CASE("audio resampler rejects invalid streams and changing frame format") {
  CodecParameters video_parameters;
  video_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  video_parameters.get()->codec_id = AV_CODEC_ID_H264;
  CHECK_THROWS_AS(
      AudioResampler(StreamInfo{0, video_parameters, AVRational{1, 1000}}),
      std::invalid_argument);

  auto invalid_audio = MakeAudioStream(48000, 2);
  invalid_audio.codec_parameters.get()->sample_rate = 0;
  CHECK_THROWS_AS(AudioResampler(std::move(invalid_audio)),
                  std::invalid_argument);

  AudioResampler resampler(MakeAudioStream(48000, 1));
  auto valid_frame = MakeFloatPlanarMonoFrame(64, 0, 48000);
  resampler.Resample(valid_frame);

  auto changed_format = MakeFloatPlanarMonoFrame(64, 64, 48000);
  changed_format->format = AV_SAMPLE_FMT_S16P;
  CHECK_THROWS_AS(resampler.Resample(changed_format), std::invalid_argument);

  auto wrong_rate = MakeFloatPlanarMonoFrame(64, 64, 44100);
  CHECK_THROWS_AS(resampler.Resample(wrong_rate), std::invalid_argument);
}

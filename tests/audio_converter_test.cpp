#include "mw/streamer/internal/audio_converter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

ffmpeg::Frame MakeMonoPlanarFrame(int sample_rate, int frame_count, int64_t pts,
                                  float first_sample) {
  ffmpeg::Frame frame;
  AVFrame* raw_frame = frame.get();
  raw_frame->format = AV_SAMPLE_FMT_FLTP;
  raw_frame->sample_rate = sample_rate;
  raw_frame->nb_samples = frame_count;
  raw_frame->pts = pts;
  raw_frame->time_base = AVRational{1, sample_rate};
  av_channel_layout_default(&raw_frame->ch_layout, 1);
  CheckFfmpeg(av_frame_get_buffer(raw_frame, 0),
              "failed to allocate test audio frame");

  auto* samples = reinterpret_cast<float*>(raw_frame->data[0]);
  for (int index = 0; index < frame_count; ++index) {
    samples[index] = first_sample + static_cast<float>(index) * 0.001F;
  }
  return frame;
}

TEST(AudioConverterTest, ConvertsMonoPlanarToStereoInterleaved) {
  ffmpeg::Frame input = MakeMonoPlanarFrame(48'000, 32, 480, 0.25F);

  AudioConverter converter;
  auto output = converter.Convert(input);

  ASSERT_TRUE(output.has_value());
  ASSERT_EQ(output->frame_count, 32U);
  ASSERT_EQ(output->samples.size(), 64U);
  EXPECT_EQ(output->pts, 480);
  EXPECT_EQ(output->time_base.numerator, 1);
  EXPECT_EQ(output->time_base.denominator, 48'000);
  EXPECT_EQ(output->pts_us, 10'000);
  constexpr float kMonoToStereoScale = 0.7071067811865476F;
  for (std::size_t frame = 0; frame < output->frame_count; ++frame) {
    EXPECT_FLOAT_EQ(output->samples[frame * 2], output->samples[frame * 2 + 1]);
    EXPECT_NEAR(
        output->samples[frame * 2],
        (0.25F + static_cast<float>(frame) * 0.001F) * kMonoToStereoScale,
        1e-6F);
  }

  const MwAudioFrameView view = output->view();
  EXPECT_EQ(view.samples, output->samples.data());
  EXPECT_EQ(view.frame_count, output->frame_count);
  EXPECT_EQ(view.sample_rate, 48'000);
  EXPECT_EQ(view.channel_count, 2);
}

TEST(AudioConverterTest, ResamplesChunksAndDrainsWithoutLosingSamples) {
  AudioConverter converter;
  std::vector<NormalizedAudioFrame> outputs;

  for (int chunk = 0; chunk < 2; ++chunk) {
    ffmpeg::Frame input = MakeMonoPlanarFrame(44'100, 2'205, chunk * 2'205,
                                              static_cast<float>(chunk));
    auto output = converter.Convert(input);
    if (output.has_value()) {
      outputs.push_back(std::move(*output));
    }
  }

  while (auto output = converter.Flush()) {
    ASSERT_GT(output->frame_count, 0U);
    outputs.push_back(std::move(*output));
  }

  std::size_t total_frames = 0;
  int64_t expected_pts_us = 0;
  for (const NormalizedAudioFrame& output : outputs) {
    EXPECT_EQ(output.pts_us, expected_pts_us);
    EXPECT_EQ(output.samples.size(), output.frame_count * 2);
    for (float sample : output.samples) {
      EXPECT_TRUE(std::isfinite(sample));
    }
    total_frames += output.frame_count;
    expected_pts_us =
        av_rescale_q(static_cast<int64_t>(total_frames), AVRational{1, 48'000},
                     AVRational{1, 1'000'000});
  }
  EXPECT_EQ(total_frames, 4'800U);
}

}  // namespace
}  // namespace mw::streamer::internal

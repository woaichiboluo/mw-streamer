#include "mw/streamer/internal/audio_fifo.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "gtest/gtest.h"

namespace mw::streamer::internal {
namespace {

NormalizedAudioFrame MakeFrame(std::size_t frame_count, int64_t pts,
                               float first_sample) {
  NormalizedAudioFrame frame;
  frame.samples.resize(frame_count * 2);
  frame.frame_count = frame_count;
  frame.pts = pts;
  frame.time_base = {1, 48'000};
  frame.pts_us = pts * 1'000'000 / 48'000;
  for (std::size_t index = 0; index < frame_count; ++index) {
    frame.samples[index * 2] = first_sample + static_cast<float>(index);
    frame.samples[index * 2 + 1] = first_sample + static_cast<float>(index);
  }
  return frame;
}

TEST(AudioFifoTest, AssemblesOneBlockAcrossInputFrames) {
  AudioFifo fifo;
  fifo.Push(MakeFrame(400, 0, 0.0F));
  fifo.Push(MakeFrame(624, 400, 400.0F));

  std::optional<NormalizedAudioFrame> output = fifo.Read();

  ASSERT_TRUE(output.has_value());
  ASSERT_EQ(output->frame_count, 1'024U);
  EXPECT_EQ(fifo.buffered_frame_count(), 0U);
  EXPECT_EQ(output->pts, 0);
  for (std::size_t index = 0; index < output->frame_count; ++index) {
    EXPECT_FLOAT_EQ(output->samples[index * 2], static_cast<float>(index));
    EXPECT_FLOAT_EQ(output->samples[index * 2 + 1], static_cast<float>(index));
  }
}

TEST(AudioFifoTest, PadsOnlyTheFinalPartialBlockWithSilence) {
  AudioFifo fifo(4, 16);
  fifo.Push(MakeFrame(3, 48, 10.0F));

  EXPECT_FALSE(fifo.Read().has_value());

  std::optional<NormalizedAudioFrame> output = fifo.Flush();
  ASSERT_TRUE(output.has_value());
  ASSERT_EQ(output->frame_count, 4U);
  EXPECT_EQ(output->pts, 48);
  EXPECT_FLOAT_EQ(output->samples[0], 10.0F);
  EXPECT_FLOAT_EQ(output->samples[2], 11.0F);
  EXPECT_FLOAT_EQ(output->samples[4], 12.0F);
  EXPECT_FLOAT_EQ(output->samples[6], 0.0F);
  EXPECT_FLOAT_EQ(output->samples[7], 0.0F);

  EXPECT_FALSE(fifo.Flush().has_value());
}

TEST(AudioFifoTest, CropsSamplesWhoseTimestampsOverlap) {
  AudioFifo fifo(4, 16);
  fifo.Push(MakeFrame(4, 0, 0.0F));
  fifo.Push(MakeFrame(4, 2, 100.0F));
  EXPECT_EQ(fifo.buffered_frame_count(), 6U);

  std::optional<NormalizedAudioFrame> output = fifo.Read();
  ASSERT_TRUE(output.has_value());
  EXPECT_FLOAT_EQ(output->samples[0], 0.0F);
  EXPECT_FLOAT_EQ(output->samples[6], 3.0F);

  output = fifo.Flush();
  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(output->pts, 4);
  EXPECT_FLOAT_EQ(output->samples[0], 102.0F);
  EXPECT_FLOAT_EQ(output->samples[2], 103.0F);
  EXPECT_FLOAT_EQ(output->samples[4], 0.0F);
  EXPECT_FLOAT_EQ(output->samples[6], 0.0F);
}

}  // namespace
}  // namespace mw::streamer::internal

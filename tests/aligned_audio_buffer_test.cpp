#include "mw/streamer/internal/aligned_audio_buffer.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "gtest/gtest.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

NormalizedAudioFrame MakeFrame(std::size_t frame_count, int64_t pts,
                               float first_sample) {
  NormalizedAudioFrame frame;
  frame.samples.resize(frame_count * kProcessorAudioChannelCount);
  frame.frame_count = frame_count;
  frame.pts = pts;
  frame.time_base = {1, kProcessorAudioSampleRate};
  frame.pts_us = pts * 1'000'000 / kProcessorAudioSampleRate;
  for (std::size_t index = 0; index < frame_count; ++index) {
    frame.samples[index * kProcessorAudioChannelCount] =
        first_sample + static_cast<float>(index);
    frame.samples[index * kProcessorAudioChannelCount + 1] =
        first_sample + static_cast<float>(index);
  }
  return frame;
}

void ExpectStereoFrame(const NormalizedAudioFrame& frame,
                       std::size_t frame_index, float expected) {
  EXPECT_FLOAT_EQ(frame.samples[frame_index * kProcessorAudioChannelCount],
                  expected);
  EXPECT_FLOAT_EQ(frame.samples[frame_index * kProcessorAudioChannelCount + 1],
                  expected);
}

TEST(AlignedAudioBufferTest, ReadsCompleteInterval) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(4, 100, 10.0F));

  const NormalizedAudioFrame output = buffer.Read(100, 4);

  EXPECT_EQ(output.frame_count, 4U);
  EXPECT_EQ(output.pts, 100);
  EXPECT_EQ(output.time_base.numerator, 1);
  EXPECT_EQ(output.time_base.denominator, kProcessorAudioSampleRate);
  EXPECT_EQ(buffer.buffered_frame_count(), 0U);
  for (std::size_t index = 0; index < 4; ++index) {
    ExpectStereoFrame(output, index, 10.0F + static_cast<float>(index));
  }
}

TEST(AlignedAudioBufferTest, JoinsSamplesAcrossFrames) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(2, 10, 1.0F));
  buffer.Push(MakeFrame(3, 12, 3.0F));

  const NormalizedAudioFrame output = buffer.Read(10, 5);

  for (std::size_t index = 0; index < 5; ++index) {
    ExpectStereoFrame(output, index, 1.0F + static_cast<float>(index));
  }
}

TEST(AlignedAudioBufferTest, RescalesOriginalPtsToAudioFrameTimeBase) {
  AlignedAudioBuffer buffer(1'024);
  NormalizedAudioFrame frame = MakeFrame(2, 10, 30.0F);
  frame.time_base = {1, 1'000};
  frame.pts_us = 10'000;
  buffer.Push(std::move(frame));

  const NormalizedAudioFrame output = buffer.Read(480, 2);

  EXPECT_EQ(output.pts, 480);
  EXPECT_EQ(output.pts_us, 10'000);
  ExpectStereoFrame(output, 0, 30.0F);
  ExpectStereoFrame(output, 1, 31.0F);
}

TEST(AlignedAudioBufferTest, PadsLeadingGapWithSilence) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(2, 12, 5.0F));

  const NormalizedAudioFrame output = buffer.Read(10, 4);

  ExpectStereoFrame(output, 0, 0.0F);
  ExpectStereoFrame(output, 1, 0.0F);
  ExpectStereoFrame(output, 2, 5.0F);
  ExpectStereoFrame(output, 3, 6.0F);
}

TEST(AlignedAudioBufferTest, PadsInternalGapWithSilence) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(2, 10, 1.0F));
  buffer.Push(MakeFrame(2, 14, 5.0F));

  const NormalizedAudioFrame output = buffer.Read(10, 6);

  ExpectStereoFrame(output, 0, 1.0F);
  ExpectStereoFrame(output, 1, 2.0F);
  ExpectStereoFrame(output, 2, 0.0F);
  ExpectStereoFrame(output, 3, 0.0F);
  ExpectStereoFrame(output, 4, 5.0F);
  ExpectStereoFrame(output, 5, 6.0F);
}

TEST(AlignedAudioBufferTest, PadsTrailingGapWithSilence) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(2, 10, 7.0F));

  const NormalizedAudioFrame output = buffer.Read(10, 4);

  ExpectStereoFrame(output, 0, 7.0F);
  ExpectStereoFrame(output, 1, 8.0F);
  ExpectStereoFrame(output, 2, 0.0F);
  ExpectStereoFrame(output, 3, 0.0F);
}

TEST(AlignedAudioBufferTest, CropsOverlappingInputSamples) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(4, 0, 0.0F));
  buffer.Push(MakeFrame(4, 2, 100.0F));

  const NormalizedAudioFrame output = buffer.Read(0, 6);

  for (std::size_t index = 0; index < 4; ++index) {
    ExpectStereoFrame(output, index, static_cast<float>(index));
  }
  ExpectStereoFrame(output, 4, 102.0F);
  ExpectStereoFrame(output, 5, 103.0F);
}

TEST(AlignedAudioBufferTest, RetainsFutureSamplesForLaterRead) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(4, 4, 20.0F));

  const NormalizedAudioFrame first = buffer.Read(0, 4);
  EXPECT_EQ(buffer.buffered_frame_count(), 4U);
  for (std::size_t index = 0; index < 4; ++index) {
    ExpectStereoFrame(first, index, 0.0F);
  }

  const NormalizedAudioFrame second = buffer.Read(4, 4);
  EXPECT_EQ(buffer.buffered_frame_count(), 0U);
  for (std::size_t index = 0; index < 4; ++index) {
    ExpectStereoFrame(second, index, 20.0F + static_cast<float>(index));
  }
}

TEST(AlignedAudioBufferTest, DiscardsInputThatMovesEntirelyBackward) {
  AlignedAudioBuffer buffer(16);
  buffer.Push(MakeFrame(4, 10, 1.0F));
  buffer.Push(MakeFrame(2, 8, 100.0F));

  EXPECT_EQ(buffer.buffered_frame_count(), 4U);
  const NormalizedAudioFrame output = buffer.Read(10, 4);
  for (std::size_t index = 0; index < 4; ++index) {
    ExpectStereoFrame(output, index, 1.0F + static_cast<float>(index));
  }
}

TEST(AlignedAudioBufferTest, ThrowsWhenCapacityWouldBeExceeded) {
  AlignedAudioBuffer buffer(4);
  buffer.Push(MakeFrame(4, 0, 1.0F));

  try {
    buffer.Push(MakeFrame(1, 4, 5.0F));
    FAIL() << "expected capacity error";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kUnavailable);
  }
  EXPECT_EQ(buffer.buffered_frame_count(), 4U);
}

TEST(AlignedAudioBufferTest, DropsOldestSamplesWhenRequested) {
  AlignedAudioBuffer buffer(4);
  buffer.Push(MakeFrame(4, 0, 1.0F));

  buffer.PushDroppingOldest(MakeFrame(2, 4, 5.0F));

  EXPECT_EQ(buffer.buffered_frame_count(), 4U);
  const NormalizedAudioFrame output = buffer.Read(0, 6);
  ExpectStereoFrame(output, 0, 0.0F);
  ExpectStereoFrame(output, 1, 0.0F);
  for (std::size_t index = 2; index < 6; ++index) {
    ExpectStereoFrame(output, index, static_cast<float>(index + 1));
  }
}

TEST(AlignedAudioBufferTest, RetainsNewestTailWhenFrameExceedsCapacity) {
  AlignedAudioBuffer buffer(4);

  buffer.PushDroppingOldest(MakeFrame(6, 0, 1.0F));

  EXPECT_EQ(buffer.buffered_frame_count(), 4U);
  const NormalizedAudioFrame output = buffer.Read(0, 6);
  ExpectStereoFrame(output, 0, 0.0F);
  ExpectStereoFrame(output, 1, 0.0F);
  for (std::size_t index = 2; index < 6; ++index) {
    ExpectStereoFrame(output, index, static_cast<float>(index + 1));
  }
}

TEST(AlignedAudioBufferTest, RejectsInputWithoutOriginalPts) {
  AlignedAudioBuffer buffer(4);
  NormalizedAudioFrame frame = MakeFrame(1, 0, 1.0F);
  frame.pts = MW_TIMESTAMP_UNAVAILABLE;
  frame.pts_us = 0;

  try {
    buffer.Push(std::move(frame));
    FAIL() << "expected unsupported timestamp error";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kUnsupported);
  }
}

}  // namespace
}  // namespace mw::streamer::internal

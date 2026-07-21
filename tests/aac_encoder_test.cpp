#include "mw/streamer/internal/aac_encoder.h"

#include <cmath>
#include <cstdint>
#include <optional>

#include "gtest/gtest.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

NormalizedAudioFrame MakeTestAudioBlock(int block_index) {
  NormalizedAudioFrame frame;
  frame.frame_count = kProcessorAudioBlockFrameCount;
  frame.samples.resize(kProcessorAudioBlockFrameCount *
                       kProcessorAudioChannelCount);
  constexpr double kPi = 3.14159265358979323846;
  for (std::size_t index = 0; index < kProcessorAudioBlockFrameCount; ++index) {
    const double sample_index = static_cast<double>(
        block_index * static_cast<int>(kProcessorAudioBlockFrameCount) +
        static_cast<int>(index));
    frame.samples[index * kProcessorAudioChannelCount] = static_cast<float>(
        std::sin(2.0 * kPi * 440.0 * sample_index / kProcessorAudioSampleRate));
    frame.samples[index * kProcessorAudioChannelCount + 1] = static_cast<float>(
        std::sin(2.0 * kPi * 880.0 * sample_index / kProcessorAudioSampleRate));
  }
  return frame;
}

void ReceiveAvailable(AacEncoder* encoder, int* packet_count,
                      std::optional<std::int64_t>* previous_dts) {
  while (auto packet = encoder->Receive()) {
    EXPECT_GT(packet->get()->size, 0);
    EXPECT_EQ(packet->get()->time_base.num, 1);
    EXPECT_EQ(packet->get()->time_base.den, kProcessorAudioSampleRate);
    if (previous_dts->has_value()) {
      EXPECT_GT(packet->get()->dts, **previous_dts);
    }
    *previous_dts = packet->get()->dts;
    ++*packet_count;
  }
}

TEST(AacEncoderTest, EncodesNormalizedInterleavedAudio) {
  AacEncoder encoder;
  ASSERT_NO_THROW(encoder.Open(128'000));
  ASSERT_TRUE(encoder.is_open());

  const AVCodecParameters* parameters = encoder.codec_parameters().get();
  ASSERT_NE(parameters, nullptr);
  EXPECT_EQ(parameters->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(parameters->profile, AV_PROFILE_AAC_LOW);
  EXPECT_EQ(parameters->sample_rate, kProcessorAudioSampleRate);
  EXPECT_EQ(parameters->ch_layout.nb_channels, kProcessorAudioChannelCount);
  EXPECT_GT(parameters->extradata_size, 0);

  int packet_count = 0;
  std::optional<std::int64_t> previous_dts;
  constexpr int kBlockCount = 8;
  for (int index = 0; index < kBlockCount; ++index) {
    NormalizedAudioFrame block = MakeTestAudioBlock(index);
    while (!encoder.Send(block)) {
      ReceiveAvailable(&encoder, &packet_count, &previous_dts);
    }
    ReceiveAvailable(&encoder, &packet_count, &previous_dts);
  }

  while (!encoder.SendEndOfStream()) {
    ReceiveAvailable(&encoder, &packet_count, &previous_dts);
  }
  ReceiveAvailable(&encoder, &packet_count, &previous_dts);

  EXPECT_GT(packet_count, 0);
}

TEST(AacEncoderTest, RejectsInvalidInputBlockSize) {
  AacEncoder encoder;
  ASSERT_NO_THROW(encoder.Open(128'000));
  NormalizedAudioFrame frame;
  frame.frame_count = 1;
  frame.samples.resize(kProcessorAudioChannelCount);

  EXPECT_THROW(static_cast<void>(encoder.Send(frame)), Error);
}

}  // namespace
}  // namespace mw::streamer::internal

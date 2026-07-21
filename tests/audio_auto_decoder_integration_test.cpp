#include "mw/streamer/internal/audio_auto_decoder.h"

#include <filesystem>
#include <utility>

#include "gtest/gtest.h"

extern "C" {
#include <libavutil/frame.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/audio_fifo.h"
#include "mw/streamer/internal/demuxer.h"

namespace mw::streamer::internal {
namespace {

struct AudioTestState {
  AudioConverter converter;
  AudioFifo fifo;
  int decoded_frames = 0;
  int decoded_samples = 0;
  int normalized_blocks = 0;
};

void ReadCompleteBlocks(AudioTestState* state) {
  while (auto block = state->fifo.Read()) {
    EXPECT_EQ(block->frame_count, kProcessorAudioBlockFrameCount);
    EXPECT_EQ(block->samples.size(),
              kProcessorAudioBlockFrameCount * kProcessorAudioChannelCount);
    ++state->normalized_blocks;
  }
}

void ConvertAndBuffer(const ffmpeg::Frame& frame, AudioTestState* state) {
  auto normalized = state->converter.Convert(frame);
  if (!normalized.has_value()) {
    return;
  }
  state->fifo.Push(std::move(*normalized));
  ReadCompleteBlocks(state);
}

void ReceiveAudio(AudioAutoDecoder* decoder, AudioTestState* state) {
  while (auto frame = decoder->Receive()) {
    const AVFrame* raw_frame = frame->get();
    ASSERT_NE(raw_frame, nullptr);
    EXPECT_EQ(raw_frame->sample_rate, 44100);
    EXPECT_EQ(raw_frame->ch_layout.nb_channels, 1);
    EXPECT_GT(raw_frame->nb_samples, 0);
    EXPECT_NE(raw_frame->pts, AV_NOPTS_VALUE);
    EXPECT_EQ(raw_frame->time_base.num, 1);
    EXPECT_GT(raw_frame->time_base.den, 0);
    ++state->decoded_frames;
    state->decoded_samples += raw_frame->nb_samples;
    ConvertAndBuffer(*frame, state);
  }
}

void FlushNormalizedAudio(AudioTestState* state) {
  while (auto normalized = state->converter.Flush()) {
    state->fifo.Push(std::move(*normalized));
    ReadCompleteBlocks(state);
  }

  while (auto block = state->fifo.Flush()) {
    EXPECT_EQ(block->frame_count, kProcessorAudioBlockFrameCount);
    ++state->normalized_blocks;
  }
}

TEST(AudioAutoDecoderIntegrationTest, SelectsAndDrainsInputDecoder) {
  const std::filesystem::path input_path =
      std::filesystem::path(MW_STREAMER_TEST_DATA_DIR) /
      "h264_aac_mono_44100.mp4";
  ASSERT_TRUE(std::filesystem::is_regular_file(input_path));

  Demuxer demuxer;
  ASSERT_NO_THROW(demuxer.Open(input_path.string()));
  ASSERT_TRUE(demuxer.has_audio());

  AudioAutoDecoder decoder;
  ASSERT_NO_THROW(decoder.Open(demuxer.audio_stream()));

  AudioTestState test_state;
  while (auto packet = demuxer.Read()) {
    if (packet->get()->stream_index != decoder.stream_index()) {
      continue;
    }

    while (!decoder.Send(*packet)) {
      ReceiveAudio(&decoder, &test_state);
    }
    ReceiveAudio(&decoder, &test_state);
  }

  while (!decoder.SendEndOfStream()) {
    ReceiveAudio(&decoder, &test_state);
  }
  ReceiveAudio(&decoder, &test_state);
  FlushNormalizedAudio(&test_state);

  EXPECT_GT(test_state.decoded_frames, 0);
  EXPECT_GT(test_state.decoded_samples, 0);
  EXPECT_GT(test_state.normalized_blocks, 0);
}

}  // namespace
}  // namespace mw::streamer::internal

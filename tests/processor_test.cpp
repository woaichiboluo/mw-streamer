#include "mw/streamer/internal/processor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/processor.h"

namespace mw::streamer::internal {
namespace {

struct CallbackState {
  const MwProcessorContext* context = nullptr;
  std::string config;
  std::size_t video_input_count = 0;
  std::size_t audio_input_count = 0;
  bool stopped = false;
};

int OnStart(const MwProcessorContext* context, const char* config, char*,
            std::size_t, void* user_data) {
  auto* state = static_cast<CallbackState*>(user_data);
  state->context = context;
  state->config = config;
  return 0;
}

void OnVideo(const MwGpuNv12FrameView*, std::size_t input_count,
             MwGpuNv12FrameView* output, void* user_data) {
  auto* state = static_cast<CallbackState*>(user_data);
  state->video_input_count = input_count;
  output->y[0] = 23;
}

void OnAudio(const MwAudioFrameView*, std::size_t input_count,
             MwAudioFrameView* output, void* user_data) {
  auto* state = static_cast<CallbackState*>(user_data);
  state->audio_input_count = input_count;
  std::fill_n(output->samples, output->frame_count * output->channel_count,
              0.5F);
}

int OnConfigError(const char*, char* error_buffer,
                  std::size_t error_buffer_capacity, void*) {
  const char message[] = "invalid model configuration";
  std::memcpy(error_buffer, message,
              std::min(error_buffer_capacity, sizeof(message)));
  return 1;
}

int OnStartWithoutMessage(const MwProcessorContext*, const char*, char*,
                          std::size_t, void*) {
  return -1;
}

void OnStop(void* user_data) {
  static_cast<CallbackState*>(user_data)->stopped = true;
}

MwProcessorContext MakeContext(const MwInputSourceInfo* inputs,
                               std::size_t input_count) {
  MwAudioOutputInfo audio_output{};
  for (std::size_t index = 0; index < input_count; ++index) {
    if (inputs[index].audio.present != 0) {
      audio_output.sample_rate = 48000;
      audio_output.channel_count = 2;
      audio_output.block_frame_count = 1024;
      break;
    }
  }
  MwProcessorContext context{};
  context.device_id = 2;
  context.inputs = inputs;
  context.input_count = input_count;
  context.video_output.width = 1920;
  context.video_output.height = 1080;
  context.video_output.frame_rate = {25, 1};
  context.video_output.color_range = MW_VIDEO_COLOR_RANGE_LIMITED;
  context.video_output.color_space = MW_VIDEO_COLOR_SPACE_BT709;
  context.audio_output = audio_output;
  return context;
}

TEST(ProcessorTest, CopiesContextAndInvokesCallbacksWithUserDataLast) {
  std::array<MwInputSourceInfo, 1> sources{};
  sources[0].input_index = 0;
  sources[0].video.present = 1;
  sources[0].video.width = 1280;
  sources[0].video.height = 720;
  sources[0].video.frame_rate = {30, 1};
  sources[0].video.color_range = MW_VIDEO_COLOR_RANGE_FULL;
  sources[0].video.color_space = MW_VIDEO_COLOR_SPACE_BT709;
  sources[0].audio.present = 1;
  sources[0].audio.sample_rate = 44100;
  sources[0].audio.channel_count = 1;
  CallbackState state;
  MwProcessorCallbacks callbacks{};
  callbacks.on_start = OnStart;
  callbacks.on_video = OnVideo;
  callbacks.on_audio = OnAudio;
  callbacks.on_stop = OnStop;
  callbacks.user_data = &state;
  Processor processor(callbacks, MakeContext(sources.data(), sources.size()));
  EXPECT_TRUE(processor.has_video_callback());
  EXPECT_TRUE(processor.has_audio_callback());
  sources[0].video.width = 1;

  processor.Start("initial");
  ASSERT_NE(state.context, nullptr);
  EXPECT_EQ(state.context->inputs[0].video.width, 1280);
  EXPECT_EQ(state.config, "initial");

  std::array<std::uint8_t, 4> y{};
  std::array<MwGpuNv12FrameView, 1> video_inputs{};
  MwGpuNv12FrameView video_output{};
  video_output.y = y.data();
  processor.ProcessVideo(video_inputs.data(), video_inputs.size(),
                         &video_output);
  EXPECT_EQ(state.video_input_count, 1);
  EXPECT_EQ(y[0], 23);

  std::array<float, 8> samples{};
  std::array<MwAudioFrameView, 1> audio_inputs{};
  MwAudioFrameView audio_output{};
  audio_output.samples = samples.data();
  audio_output.frame_count = 4;
  audio_output.channel_count = 2;
  processor.ProcessAudio(audio_inputs.data(), audio_inputs.size(),
                         &audio_output);
  EXPECT_EQ(state.audio_input_count, 1);
  EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                          [](float value) { return value == 0.5F; }));

  processor.Stop();
  EXPECT_TRUE(state.stopped);
}

TEST(ProcessorTest, UsesBlackPrefillAndSilenceWhenProcessCallbacksAreNull) {
  Processor processor({}, MakeContext(nullptr, 0));
  EXPECT_FALSE(processor.has_video_callback());
  EXPECT_FALSE(processor.has_audio_callback());
  processor.Start("");

  std::array<std::uint8_t, 4> black_y{16, 16, 16, 16};
  MwGpuNv12FrameView video_output{};
  video_output.y = black_y.data();
  processor.ProcessVideo(nullptr, 0, &video_output);
  EXPECT_TRUE(std::all_of(black_y.begin(), black_y.end(),
                          [](std::uint8_t value) { return value == 16; }));

  std::array<float, 8> samples;
  samples.fill(1.0F);
  MwAudioFrameView audio_output{};
  audio_output.samples = samples.data();
  audio_output.frame_count = 4;
  audio_output.channel_count = 2;
  processor.ProcessAudio(nullptr, 0, &audio_output);
  EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                          [](float value) { return value == 0.0F; }));
}

TEST(ProcessorTest, ThrowsDedicatedErrorsForCallbackFailures) {
  MwProcessorCallbacks callbacks{};
  callbacks.on_start = OnStartWithoutMessage;
  callbacks.on_config_update = OnConfigError;
  Processor processor(callbacks, MakeContext(nullptr, 0));

  try {
    processor.Start("");
    FAIL() << "Processor::Start returned";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kProcessorStartCallbackFailed);
    EXPECT_STREQ(error.what(), "Processor on_start callback failed");
  }

  try {
    processor.UpdateConfig("bad");
    FAIL() << "Processor::UpdateConfig returned";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), StatusCode::kProcessorConfigUpdateCallbackFailed);
    EXPECT_STREQ(error.what(), "invalid model configuration");
  }
}

}  // namespace
}  // namespace mw::streamer::internal

#include <stdint.h>
#include <string.h>

#include "mw/processor/processor.h"

typedef struct TestProcessor {
  uint32_t start_calls;
  uint32_t video_calls;
  uint32_t audio_calls;
  uint32_t update_calls;
  uint32_t stop_calls;
} TestProcessor;

static MwStreamerProcessorStartResult OnStart(
    const MwStreamerProcessorStartRequest* request, void* user_context) {
  TestProcessor* processor = user_context;
  if (!request->source_info->has_video || !request->source_info->has_audio ||
      request->source_info->video.codec != kMwStreamerCodecH264 ||
      request->source_info->audio.codec != kMwStreamerCodecAac ||
      request->config->output_width != 1920 ||
      strcmp(request->config->config, "initial") != 0 ||
      request->execution->type != kMwStreamerExecutionCuda) {
    return kMwStreamerProcessorStartFailed;
  }
  ++processor->start_calls;
  return kMwStreamerProcessorStartSuccess;
}

static void ProcessVideo(const MwStreamerVideoProcessRequest* request,
                         void* user_context) {
  TestProcessor* processor = user_context;
  if (request->input->buffer.width == 3840 && request->output.width == 1920 &&
      request->input->timestamp.pts == 9000 &&
      request->execution->type == kMwStreamerExecutionCuda) {
    ++processor->video_calls;
  }
}

static void ProcessAudio(const MwStreamerAudioProcessRequest* request,
                         void* user_context) {
  TestProcessor* processor = user_context;
  const size_t sample_count =
      request->input->channel_count * request->input->samples_per_channel;
  for (size_t index = 0; index < sample_count; ++index) {
    request->output.data[index] = request->input->data[index];
  }
  if (request->input->sample_rate == 48000 &&
      request->output.channel_count == request->input->channel_count &&
      request->output.samples_per_channel ==
          request->input->samples_per_channel) {
    ++processor->audio_calls;
  }
}

static void UpdateConfig(const char* config, void* user_context) {
  TestProcessor* processor = user_context;
  if (strcmp(config, "updated") == 0) {
    ++processor->update_calls;
  }
}

static void OnStop(void* user_context) {
  TestProcessor* processor = user_context;
  ++processor->stop_calls;
}

int main(void) {
  TestProcessor processor = {0};
  MwStreamerProcessorCallbacks callbacks = {
      .user_context = &processor,
      .on_start = OnStart,
      .process_video = ProcessVideo,
      .process_audio = ProcessAudio,
      .update_config = UpdateConfig,
      .on_stop = OnStop,
  };
  const MwStreamerProcessorConfig config = {
      .output_width = 1920,
      .output_height = 1080,
      .config = "initial",
  };
  const MwStreamerProcessorSourceInfo source_info = {
      .has_video = 1,
      .has_audio = 1,
      .video =
          {
              .codec = kMwStreamerCodecH264,
              .width = 3840,
              .height = 2160,
              .pixel_format = kMwStreamerVideoPixelFormatNv12,
              .frame_rate = {.num = 25, .den = 1},
              .time_base = {.num = 1, .den = 90000},
          },
      .audio =
          {
              .codec = kMwStreamerCodecAac,
              .sample_rate = 44100,
              .channel_count = 2,
              .time_base = {.num = 1, .den = 44100},
          },
  };
  const MwStreamerVideoPlaneView input_planes[] = {
      {.address = 1, .stride = 4096},
      {.address = 2, .stride = 4096},
  };
  const MwStreamerVideoPlaneView output_planes[] = {
      {.address = 3, .stride = 2048},
      {.address = 4, .stride = 2048},
  };
  const MwStreamerVideoFrameView video_input = {
      .buffer =
          {
              .memory_type = kMwStreamerMemoryCuda,
              .pixel_format = kMwStreamerVideoPixelFormatNv12,
              .width = 3840,
              .height = 2160,
              .planes = input_planes,
              .plane_count = 2,
          },
      .timestamp =
          {
              .pts = 9000,
              .duration = 3600,
              .time_base = {.num = 1, .den = 90000},
          },
  };
  const MwStreamerExecutionContext execution = {
      .type = kMwStreamerExecutionCuda,
      .native_handle = (uintptr_t)5,
  };
  const MwStreamerProcessorStartRequest start_request = {
      .source_info = &source_info,
      .config = &config,
      .execution = &execution,
  };
  if (callbacks.on_start(&start_request, callbacks.user_context) !=
      kMwStreamerProcessorStartSuccess) {
    return 1;
  }

  const MwStreamerVideoProcessRequest video_request = {
      .input = &video_input,
      .output =
          {
              .memory_type = kMwStreamerMemoryCuda,
              .pixel_format = kMwStreamerVideoPixelFormatNv12,
              .width = config.output_width,
              .height = config.output_height,
              .planes = output_planes,
              .plane_count = 2,
          },
      .execution = &execution,
  };
  callbacks.process_video(&video_request, callbacks.user_context);

  const float audio_input_data[] = {0.25F, -0.25F, 0.5F, -0.5F};
  float audio_output_data[4] = {0};
  const MwStreamerAudioFrameView audio_input = {
      .data = audio_input_data,
      .sample_rate = 48000,
      .channel_count = 2,
      .samples_per_channel = 2,
      .timestamp =
          {
              .pts = 0,
              .duration = 2,
              .time_base = {.num = 1, .den = 48000},
          },
  };
  const MwStreamerAudioProcessRequest audio_request = {
      .input = &audio_input,
      .output =
          {
              .data = audio_output_data,
              .channel_count = 2,
              .samples_per_channel = 2,
          },
  };
  callbacks.process_audio(&audio_request, callbacks.user_context);

  callbacks.update_config("updated", callbacks.user_context);
  callbacks.on_stop(callbacks.user_context);

  if (processor.start_calls != 1 || processor.video_calls != 1 ||
      processor.audio_calls != 1 || processor.update_calls != 1 ||
      processor.stop_calls != 1) {
    return 1;
  }
  if (memcmp(audio_input_data, audio_output_data, sizeof(audio_input_data)) !=
      0) {
    return 1;
  }
  return 0;
}

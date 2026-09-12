#include <stdint.h>
#include <string.h>

#include "mw/streamer/processor/processor.h"

typedef struct TestProcessor {
  uint32_t start_calls;
  uint32_t video_calls;
  uint32_t audio_calls;
  uint32_t boundary_calls;
  uint32_t update_calls;
  uint32_t stop_calls;
  MwStreamerExecutionContext execution;
} TestProcessor;

static MwStreamerProcessorStartResult OnStart(
    const MwStreamerTransformProcessorStartRequest* request,
    void* user_context) {
  TestProcessor* processor = user_context;
  if (!request->source_info->has_video || !request->source_info->has_audio ||
      request->source_info->video.codec != kMwStreamerCodecH264 ||
      request->source_info->audio.codec != kMwStreamerCodecAac ||
      !request->video_output_size ||
      request->video_output_size->width != 1920 ||
      request->video_output_size->height != 1080 ||
      strcmp(request->config->config, "initial") != 0 ||
      request->execution->type != kMwStreamerExecutionCuda) {
    return kMwStreamerProcessorStartFailed;
  }
  request->video_output_size->width = 1280;
  request->video_output_size->height = 720;
  processor->execution = *request->execution;
  ++processor->start_calls;
  return kMwStreamerProcessorStartSuccess;
}

static void ProcessVideo(const MwStreamerTransformVideoProcessRequest* request,
                         void* user_context) {
  TestProcessor* processor = user_context;
  if (request->input->buffer.width == 3840 && request->output->width == 1280 &&
      request->output->height == 720 &&
      request->input->buffer.storage_type == kMwStreamerVideoStorageLinear &&
      request->input->buffer.storage.linear.plane_count == 2 &&
      request->input->timestamp.pts == 9000 &&
      processor->execution.type == kMwStreamerExecutionCuda) {
    ++processor->video_calls;
  }
}

static void ProcessAudio(const MwStreamerTransformAudioProcessRequest* request,
                         void* user_context) {
  TestProcessor* processor = user_context;
  const size_t sample_count =
      request->input->channel_count * request->input->samples_per_channel;
  for (size_t index = 0; index < sample_count; ++index) {
    request->output->data[index] = request->input->data[index];
  }
  if (request->input->sample_rate == 48000 &&
      request->output->channel_count == request->input->channel_count &&
      request->output->samples_per_channel ==
          request->input->samples_per_channel) {
    ++processor->audio_calls;
  }
}

static void OnBoundary(MwStreamerProcessorBoundaryReason reason,
                       void* user_context) {
  TestProcessor* processor = user_context;
  if (reason == kMwStreamerProcessorEndOfInput) {
    ++processor->boundary_calls;
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

static MwStreamerProcessorStartResult OnFileStart(
    const MwStreamerAnalysisProcessorStartRequest* request,
    void* user_context) {
  TestProcessor* processor = user_context;
  if (!request->source_info->has_video || !request->source_info->has_audio ||
      strcmp(request->config->config, "file") != 0 ||
      request->execution->type != kMwStreamerExecutionCuda) {
    return kMwStreamerProcessorStartFailed;
  }
  ++processor->start_calls;
  return kMwStreamerProcessorStartSuccess;
}

static void ProcessFileVideo(const MwStreamerVideoFrameView* input,
                             void* user_context) {
  if (input->buffer.width == 3840) {
    ++((TestProcessor*)user_context)->video_calls;
  }
}

static void ProcessFileAudio(const MwStreamerAudioFrameView* input,
                             void* user_context) {
  if (input->sample_rate == 48000) {
    ++((TestProcessor*)user_context)->audio_calls;
  }
}

int main(void) {
  TestProcessor processor = {0};
  MwStreamerTransformProcessorCallbacks callbacks = {
      .user_context = &processor,
      .on_start = OnStart,
      .process_video = ProcessVideo,
      .process_audio = ProcessAudio,
      .on_boundary = OnBoundary,
      .on_config_update = UpdateConfig,
      .on_stop = OnStop,
  };
  const MwStreamerTransformProcessorConfig config = {
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
      {.address = 1,
       .stride_bytes = 4096,
       .row_bytes = 3840,
       .row_count = 2160},
      {.address = 2,
       .stride_bytes = 4096,
       .row_bytes = 3840,
       .row_count = 1080},
  };
  const MwStreamerVideoPlaneView output_planes[] = {
      {.address = 3, .stride_bytes = 1280, .row_bytes = 1280, .row_count = 720},
      {.address = 4, .stride_bytes = 1280, .row_bytes = 1280, .row_count = 360},
  };
  const MwStreamerVideoFrameView video_input = {
      .buffer =
          {
              .memory_type = kMwStreamerMemoryCuda,
              .storage_type = kMwStreamerVideoStorageLinear,
              .pixel_format = kMwStreamerVideoPixelFormatNv12,
              .width = 3840,
              .height = 2160,
              .storage =
                  {
                      .linear =
                          {
                              .planes = input_planes,
                              .plane_count = 2,
                          },
                  },
          },
      .color =
          {
              .range = kMwStreamerColorRangeLimited,
              .space = kMwStreamerColorSpaceBt709,
              .primaries = kMwStreamerColorPrimariesBt709,
              .transfer = kMwStreamerColorTransferBt709,
              .chroma_location = kMwStreamerChromaLocationLeft,
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
  };
  MwStreamerVideoOutputSize video_output_size = {.width = 1920, .height = 1080};
  const MwStreamerTransformProcessorStartRequest start_request = {
      .source_info = &source_info,
      .config = &config,
      .execution = &execution,
      .video_output_size = &video_output_size,
  };
  if (callbacks.on_start(&start_request, callbacks.user_context) !=
      kMwStreamerProcessorStartSuccess) {
    return 1;
  }
  if (video_output_size.width != 1280 || video_output_size.height != 720) {
    return 1;
  }

  const MwStreamerTransformVideoProcessRequest video_request = {
      .input = &video_input,
      .output =
          &(MwStreamerVideoBufferView){
              .memory_type = kMwStreamerMemoryCuda,
              .storage_type = kMwStreamerVideoStorageLinear,
              .pixel_format = kMwStreamerVideoPixelFormatNv12,
              .width = video_output_size.width,
              .height = video_output_size.height,
              .storage =
                  {
                      .linear =
                          {
                              .planes = output_planes,
                              .plane_count = 2,
                          },
                  },
          },
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
  const MwStreamerTransformAudioProcessRequest audio_request = {
      .input = &audio_input,
      .output =
          &(MwStreamerAudioBufferView){
              .data = audio_output_data,
              .channel_count = 2,
              .samples_per_channel = 2,
          },
  };
  callbacks.process_audio(&audio_request, callbacks.user_context);

  callbacks.on_boundary(kMwStreamerProcessorEndOfInput, callbacks.user_context);
  callbacks.on_config_update("updated", callbacks.user_context);
  callbacks.on_stop(callbacks.user_context);

  if (processor.start_calls != 1 || processor.video_calls != 1 ||
      processor.audio_calls != 1 || processor.boundary_calls != 1 ||
      processor.update_calls != 1 || processor.stop_calls != 1) {
    return 1;
  }
  if (memcmp(audio_input_data, audio_output_data, sizeof(audio_input_data)) !=
      0) {
    return 1;
  }

  TestProcessor analysis_processor = {0};
  const MwStreamerAnalysisProcessorCallbacks analysis_callbacks = {
      .user_context = &analysis_processor,
      .on_start = OnFileStart,
      .process_video = ProcessFileVideo,
      .process_audio = ProcessFileAudio,
      .on_boundary = OnBoundary,
      .on_config_update = UpdateConfig,
      .on_stop = OnStop,
  };
  const MwStreamerAnalysisProcessorConfig analysis_config = {
      .config = "file",
  };
  const MwStreamerAnalysisProcessorStartRequest analysis_start_request = {
      .source_info = &source_info,
      .config = &analysis_config,
      .execution = &execution,
  };
  if (analysis_callbacks.on_start(&analysis_start_request,
                                  analysis_callbacks.user_context) !=
      kMwStreamerProcessorStartSuccess) {
    return 1;
  }
  analysis_callbacks.process_video(&video_input,
                                   analysis_callbacks.user_context);
  analysis_callbacks.process_audio(&audio_input,
                                   analysis_callbacks.user_context);
  analysis_callbacks.on_boundary(kMwStreamerProcessorEndOfInput,
                                 analysis_callbacks.user_context);
  analysis_callbacks.on_config_update("updated",
                                      analysis_callbacks.user_context);
  analysis_callbacks.on_stop(analysis_callbacks.user_context);
  if (analysis_processor.start_calls != 1 ||
      analysis_processor.video_calls != 1 ||
      analysis_processor.audio_calls != 1 ||
      analysis_processor.boundary_calls != 1 ||
      analysis_processor.update_calls != 1 ||
      analysis_processor.stop_calls != 1) {
    return 1;
  }
  return 0;
}

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/processor/file_processor_handler.h"
#include "mw/processor/streaming_processor_handler.h"

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::processor::FileProcessorHandler;
using mw::streamer::processor::StreamingProcessorHandler;

template <typename Function>
void CheckRuntimeError(Function&& function, std::string_view expected) {
  bool caught = false;
  try {
    function();
  } catch (const std::runtime_error& error) {
    caught = true;
    CHECK(std::string_view(error.what()) == expected);
  }
  CHECK(caught);
}

MwStreamerProcessorSourceInfo MakeSourceInfo(bool has_video = true,
                                             bool has_audio = true) {
  return {
      static_cast<std::uint8_t>(has_video),
      static_cast<std::uint8_t>(has_audio),
      {
          kMwStreamerCodecH264,
          has_video ? 64U : 0U,
          has_video ? 32U : 0U,
          {25, 1},
          {1, 90000},
      },
      {
          kMwStreamerCodecAac,
          has_audio ? 44100U : 0U,
          has_audio ? 2U : 0U,
          {1, 44100},
      },
  };
}

Frame MakeSoftwareVideoFrame(AVPixelFormat format = AV_PIX_FMT_YUV420P) {
  Frame frame;
  frame->format = format;
  frame->width = 64;
  frame->height = 32;
  frame->pts = 9000;
  frame->duration = 3600;
  frame->time_base = {1, 90000};
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  frame->color_primaries = AVCOL_PRI_BT709;
  frame->color_trc = AVCOL_TRC_BT709;
  frame->chroma_location = AVCHROMA_LOC_LEFT;
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  return frame;
}

Frame MakeAudioFrame() {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 4;
  frame->pts = 480;
  frame->duration = 4;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  auto* samples = reinterpret_cast<float*>(frame->extended_data[0]);
  for (int index = 0; index < frame->nb_samples * 2; ++index) {
    samples[index] = static_cast<float>(index) / 8.0F;
  }
  return frame;
}

struct CallbackState {
  std::uint32_t start_calls = 0;
  std::uint32_t video_calls = 0;
  std::uint32_t audio_calls = 0;
  std::uint32_t timeline_reset_calls = 0;
  std::uint32_t end_of_input_calls = 0;
  std::uint32_t update_calls = 0;
  std::uint32_t stop_calls = 0;
  std::string initial_config;
  std::string updated_config;
  MwStreamerProcessorSourceInfo source_info{};
  MwStreamerExecutionContext execution{};
};

MwStreamerProcessorStartResult OnStart(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  ++state->start_calls;
  state->initial_config = request->config->config;
  state->source_info = *request->source_info;
  state->execution = *request->execution;
  return kMwStreamerProcessorStartSuccess;
}

MwStreamerProcessorStartResult OnFileStart(
    const MwStreamerFileProcessorStartRequest* request, void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  ++state->start_calls;
  state->initial_config = request->config->config;
  state->source_info = *request->source_info;
  state->execution = *request->execution;
  return kMwStreamerProcessorStartSuccess;
}

void ProcessVideo(const MwStreamerStreamingVideoProcessRequest* request,
                  void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  ++state->video_calls;
  for (std::uint32_t plane = 0;
       plane < request->output->storage.linear.plane_count; ++plane) {
    const auto& output_plane = request->output->storage.linear.planes[plane];
    auto* data = reinterpret_cast<std::uint8_t*>(output_plane.address);
    for (std::uint32_t row = 0; row < output_plane.row_count; ++row) {
      std::memset(data + row * output_plane.stride_bytes, 0x5a,
                  output_plane.row_bytes);
    }
  }
}

void ProcessAudio(const MwStreamerStreamingAudioProcessRequest* request,
                  void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  ++state->audio_calls;
  const std::size_t sample_count =
      request->input->channel_count * request->input->samples_per_channel;
  for (std::size_t index = 0; index < sample_count; ++index) {
    request->output->data[index] = request->input->data[index] * 2.0F;
  }
}

void OnBoundary(MwStreamerProcessorBoundaryReason reason, void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  if (reason == kMwStreamerProcessorTimelineReset) {
    ++state->timeline_reset_calls;
  } else if (reason == kMwStreamerProcessorEndOfInput) {
    ++state->end_of_input_calls;
  }
}

void UpdateConfig(const char* config, void* user_context) {
  auto* state = static_cast<CallbackState*>(user_context);
  ++state->update_calls;
  state->updated_config = config;
}

void OnStop(void* user_context) {
  ++static_cast<CallbackState*>(user_context)->stop_calls;
}

MwStreamerStreamingProcessorCallbacks MakeCallbacks(CallbackState* state) {
  return {state,      OnStart,      ProcessVideo, ProcessAudio,
          OnBoundary, UpdateConfig, OnStop};
}

TEST_CASE("StreamingProcessorHandler冻结构造信息并执行完整生命周期") {
  auto source_info = MakeSourceInfo();
  StreamingProcessorHandler handler(source_info, nullptr);

  source_info.video.width = 1;

  CallbackState state;
  std::string initial_config = "initial";
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      initial_config.c_str(),
  };
  REQUIRE(handler.Start(config, MakeCallbacks(&state)) ==
          kMwStreamerProcessorStartSuccess);
  initial_config = "mutated";

  CHECK(state.start_calls == 1);
  CHECK(state.initial_config == "initial");
  CHECK(state.source_info.video.width == 64);
  CHECK(state.execution.type == kMwStreamerExecutionCpu);

  auto video_input = MakeSoftwareVideoFrame();
  video_input->crop_top = 2;
  video_input->crop_left = 4;
  auto video_output = handler.ProcessVideo(video_input);
  CHECK(state.video_calls == 1);
  CHECK(video_output->format == AV_PIX_FMT_YUV420P);
  CHECK(video_output->width == 32);
  CHECK(video_output->height == 16);
  CHECK(video_output->data[0][0] == 0x5a);
  CHECK(video_output->pts == video_input->pts);
  CHECK(video_output->duration == video_input->duration);
  CHECK(video_output->time_base.num == 1);
  CHECK(video_output->time_base.den == 90000);
  CHECK(video_output->color_range == AVCOL_RANGE_MPEG);
  CHECK(video_output->crop_top == 0);
  CHECK(video_output->crop_left == 0);

  auto audio_input = MakeAudioFrame();
  auto audio_output = handler.ProcessAudio(audio_input);
  CHECK(state.audio_calls == 1);
  REQUIRE(audio_output->extended_data[0] != audio_input->extended_data[0]);
  const auto* input_samples =
      reinterpret_cast<const float*>(audio_input->extended_data[0]);
  const auto* output_samples =
      reinterpret_cast<const float*>(audio_output->extended_data[0]);
  for (int index = 0; index < audio_input->nb_samples * 2; ++index) {
    CHECK(output_samples[index] == input_samples[index] * 2.0F);
  }
  CHECK(audio_output->pts == audio_input->pts);
  CHECK(audio_output->duration == audio_input->duration);

  handler.NotifyBoundary(kMwStreamerProcessorTimelineReset);
  handler.NotifyBoundary(kMwStreamerProcessorEndOfInput);
  handler.UpdateConfig("updated");
  handler.Stop();
  handler.Stop();

  CHECK(state.timeline_reset_calls == 1);
  CHECK(state.end_of_input_calls == 1);
  CHECK(state.update_calls == 1);
  CHECK(state.updated_config == "updated");
  CHECK(state.stop_calls == 1);
  CHECK_THROWS_AS(handler.ProcessVideo(video_input), std::logic_error);
  CHECK_THROWS_AS(handler.Start(config, MakeCallbacks(&state)),
                  std::logic_error);
}

TEST_CASE("StreamingProcessorHandler提供缓存黑帧和独立音频副本") {
  const auto source_info = MakeSourceInfo();
  StreamingProcessorHandler handler(source_info, nullptr);
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };
  REQUIRE(handler.Start(config, {}) == kMwStreamerProcessorStartSuccess);

  auto video_input = MakeSoftwareVideoFrame();
  auto first_video_output = handler.ProcessVideo(video_input);
  auto second_video_output = handler.ProcessVideo(video_input);
  CHECK(first_video_output->data[0] == second_video_output->data[0]);
  CHECK(first_video_output->data[0][0] == 16);
  CHECK(first_video_output->data[1][0] == 128);
  CHECK(first_video_output->data[2][0] == 128);

  video_input->color_range = AVCOL_RANGE_JPEG;
  auto full_range_output = handler.ProcessVideo(video_input);
  CHECK(full_range_output->data[0] != first_video_output->data[0]);
  CHECK(full_range_output->data[0][0] == 0);
  CHECK(first_video_output->data[0][0] == 16);

  auto audio_input = MakeAudioFrame();
  auto audio_output = handler.ProcessAudio(audio_input);
  REQUIRE(audio_output->extended_data[0] != audio_input->extended_data[0]);
  CHECK(std::memcmp(audio_output->extended_data[0],
                    audio_input->extended_data[0],
                    static_cast<std::size_t>(audio_input->nb_samples) * 2 *
                        sizeof(float)) == 0);

  auto changed_audio_layout = MakeAudioFrame();
  changed_audio_layout->ch_layout.u.mask = AV_CH_LAYOUT_STEREO_DOWNMIX;
  CHECK_THROWS_AS(handler.ProcessAudio(changed_audio_layout),
                  std::invalid_argument);
}

TEST_CASE("StreamingProcessorHandler拒绝运行期视频结构变化") {
  const auto source_info = MakeSourceInfo(true, false);
  StreamingProcessorHandler handler(source_info, nullptr);
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };

  auto input = MakeSoftwareVideoFrame();
  CHECK_THROWS_AS(handler.ProcessVideo(input), std::logic_error);
  REQUIRE(handler.Start(config, {}) == kMwStreamerProcessorStartSuccess);
  handler.ProcessVideo(input);

  auto changed_format = MakeSoftwareVideoFrame(AV_PIX_FMT_YUV444P);
  CHECK_THROWS_AS(handler.ProcessVideo(changed_format), std::invalid_argument);
}

TEST_CASE("StreamingProcessorHandler生成CUDA默认黑帧") {
  const auto hardware_context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
  REQUIRE(frames_ref != nullptr);

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = 64;
  frames_context->height = 32;
  REQUIRE(av_hwframe_ctx_init(frames_ref) >= 0);

  Frame input;
  const int allocation_result =
      av_hwframe_get_buffer(frames_ref, input.get(), 0);
  av_buffer_unref(&frames_ref);
  REQUIRE(allocation_result >= 0);
  input->pts = 90;
  input->duration = 1;
  input->time_base = {1, 25};
  input->color_range = AVCOL_RANGE_MPEG;

  const auto source_info = MakeSourceInfo(true, false);
  StreamingProcessorHandler handler(source_info, &hardware_context);
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };
  REQUIRE(handler.Start(config, {}) == kMwStreamerProcessorStartSuccess);

  auto output = handler.ProcessVideo(input);
  Frame software_output;
  software_output->format = AV_PIX_FMT_NV12;
  software_output->width = 32;
  software_output->height = 16;
  REQUIRE(av_hwframe_transfer_data(software_output.get(), output.get(), 0) >=
          0);

  CHECK(software_output->data[0][0] == 16);
  CHECK(software_output->data[1][0] == 128);
}

TEST_CASE("StreamingProcessorHandler拒绝其他硬件上下文分配的CUDA帧") {
  const auto processor_context = HardwareContext::CreateCuda(0);
  const auto frame_context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(frame_context.get()));
  REQUIRE(frames_ref != nullptr);

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = 64;
  frames_context->height = 32;
  REQUIRE(av_hwframe_ctx_init(frames_ref) >= 0);

  Frame input;
  const int allocation_result =
      av_hwframe_get_buffer(frames_ref, input.get(), 0);
  av_buffer_unref(&frames_ref);
  REQUIRE(allocation_result >= 0);
  input->time_base = {1, 25};

  const auto source_info = MakeSourceInfo(true, false);
  StreamingProcessorHandler handler(source_info, &processor_context);
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };
  REQUIRE(handler.Start(config, {}) == kMwStreamerProcessorStartSuccess);
  CHECK_THROWS_AS(handler.ProcessVideo(input), std::invalid_argument);
}

TEST_CASE("FileProcessorHandler的CUDA回调只接收输入帧") {
  const auto hardware_context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
  REQUIRE(frames_ref != nullptr);

  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = 64;
  frames_context->height = 32;
  REQUIRE(av_hwframe_ctx_init(frames_ref) >= 0);

  Frame input;
  const int allocation_result =
      av_hwframe_get_buffer(frames_ref, input.get(), 0);
  av_buffer_unref(&frames_ref);
  REQUIRE(allocation_result >= 0);
  input->time_base = {1, 25};

  const auto source_info = MakeSourceInfo(true, false);
  FileProcessorHandler handler(source_info, &hardware_context);
  std::uint32_t callback_count = 0;
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_count;
  callbacks.process_video = [](const MwStreamerVideoFrameView* input,
                               void* user_context) {
    CHECK(input->buffer.memory_type == kMwStreamerMemoryCuda);
    ++*static_cast<std::uint32_t*>(user_context);
  };
  const MwStreamerFileProcessorConfig config = {""};
  REQUIRE(handler.Start(config, callbacks) == kMwStreamerProcessorStartSuccess);

  handler.ProcessVideo(input);
  handler.ProcessVideo(input);
  CHECK(callback_count == 2);
}

TEST_CASE("FileProcessorHandler只投递输入并通知文件结束") {
  const auto source_info = MakeSourceInfo();
  FileProcessorHandler handler(source_info, nullptr);
  CallbackState state;
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = OnFileStart;
  callbacks.on_boundary = OnBoundary;
  callbacks.update_config = UpdateConfig;
  callbacks.on_stop = OnStop;
  callbacks.process_video = [](const MwStreamerVideoFrameView* input,
                               void* user_context) {
    auto* callback_state = static_cast<CallbackState*>(user_context);
    CHECK(input->buffer.width == 64);
    ++callback_state->video_calls;
  };
  callbacks.process_audio = [](const MwStreamerAudioFrameView* input,
                               void* user_context) {
    auto* callback_state = static_cast<CallbackState*>(user_context);
    CHECK(input->sample_rate == 48000);
    ++callback_state->audio_calls;
  };
  const MwStreamerFileProcessorConfig config = {"batch=8"};

  REQUIRE(handler.Start(config, callbacks) == kMwStreamerProcessorStartSuccess);
  CHECK(state.initial_config == "batch=8");
  handler.ProcessVideo(MakeSoftwareVideoFrame());
  handler.ProcessAudio(MakeAudioFrame());
  handler.NotifyBoundary(kMwStreamerProcessorEndOfInput);
  handler.UpdateConfig("updated");
  handler.Stop();

  CHECK(state.video_calls == 1);
  CHECK(state.audio_calls == 1);
  CHECK(state.end_of_input_calls == 1);
  CHECK(state.updated_config == "updated");
  CHECK(state.stop_calls == 1);
}

TEST_CASE("StreamingProcessorHandler传播回调异常并隔离停止回调异常") {
  const auto source_info = MakeSourceInfo();
  StreamingProcessorHandler handler(source_info, nullptr);
  std::size_t stop_calls = 0;
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &stop_calls;
  callbacks.process_video = [](const MwStreamerStreamingVideoProcessRequest*,
                               void*) {
    throw std::runtime_error("视频回调失败");
  };
  callbacks.process_audio = [](const MwStreamerStreamingAudioProcessRequest*,
                               void*) {
    throw std::runtime_error("音频回调失败");
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason, void*) {
    throw std::runtime_error("边界回调失败");
  };
  callbacks.update_config = [](const char*, void*) {
    throw std::runtime_error("更新回调失败");
  };
  callbacks.on_stop = [](void* user_context) {
    ++*static_cast<std::size_t*>(user_context);
    throw std::runtime_error("停止回调失败");
  };
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };
  REQUIRE(handler.Start(config, callbacks) == kMwStreamerProcessorStartSuccess);

  CheckRuntimeError([&]() { handler.ProcessVideo(MakeSoftwareVideoFrame()); },
                    "视频回调失败");
  CheckRuntimeError([&]() { handler.ProcessAudio(MakeAudioFrame()); },
                    "音频回调失败");
  CheckRuntimeError(
      [&]() { handler.NotifyBoundary(kMwStreamerProcessorTimelineReset); },
      "边界回调失败");
  CheckRuntimeError([&]() { handler.UpdateConfig("updated"); }, "更新回调失败");
  CHECK_NOTHROW(handler.Stop());
  CHECK(stop_calls == 1);
}

TEST_CASE("StreamingProcessorHandler传播启动异常并隔离析构停止异常") {
  const auto source_info = MakeSourceInfo(true, false);
  const MwStreamerStreamingProcessorConfig config = {
      32,
      16,
      "",
  };

  StreamingProcessorHandler failed(source_info, nullptr);
  MwStreamerStreamingProcessorCallbacks throwing_start{};
  throwing_start.on_start = [](const MwStreamerStreamingProcessorStartRequest*,
                               void*) -> MwStreamerProcessorStartResult {
    throw std::runtime_error("启动回调失败");
  };
  CheckRuntimeError([&]() { failed.Start(config, throwing_start); },
                    "启动回调失败");

  std::size_t stop_calls = 0;
  CHECK_NOTHROW([&]() {
    StreamingProcessorHandler scoped(source_info, nullptr);
    MwStreamerStreamingProcessorCallbacks callbacks{};
    callbacks.user_context = &stop_calls;
    callbacks.on_stop = [](void* user_context) {
      ++*static_cast<std::size_t*>(user_context);
      throw std::runtime_error("析构停止回调失败");
    };
    REQUIRE(scoped.Start(config, callbacks) ==
            kMwStreamerProcessorStartSuccess);
  }());
  CHECK(stop_calls == 1);
}

TEST_CASE("Streaming和File Processor使用独立配置") {
  const auto source_info = MakeSourceInfo(true, false);

  StreamingProcessorHandler streaming(source_info, nullptr);
  const MwStreamerStreamingProcessorConfig missing_streaming_output = {
      0,
      0,
      "",
  };
  CHECK_THROWS_AS(streaming.Start(missing_streaming_output, {}),
                  std::invalid_argument);

  FileProcessorHandler file(source_info, nullptr);
  const MwStreamerFileProcessorConfig file_config = {"batch=8"};
  CHECK(file.Start(file_config, {}) == kMwStreamerProcessorStartSuccess);
}

}  // namespace

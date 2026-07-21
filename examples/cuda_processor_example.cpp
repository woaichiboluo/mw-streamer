#include <cuda_runtime_api.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "fmt/format.h"
#include "mw/streamer/pipeline.h"
#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/processor.h"
#include "mw/streamer/status.h"

namespace {

enum class CopyMode {
  kIdentity,
  kVerticalStack,
};

struct ProcessorState {
  CopyMode mode = CopyMode::kIdentity;
  cudaStream_t stream = nullptr;
  std::atomic<int> callback_error{cudaSuccess};
  std::atomic<std::uint64_t> video_callback_count{0};
  std::atomic<std::uint64_t> audio_callback_count{0};
};

void SetErrorMessage(std::string_view message, char* error_buffer,
                     std::size_t error_buffer_capacity) noexcept {
  if (error_buffer == nullptr || error_buffer_capacity == 0) {
    return;
  }
  try {
    const auto result = fmt::format_to_n(
        error_buffer, error_buffer_capacity - 1, "{}", message);
    *result.out = '\0';
  } catch (...) {
    error_buffer[0] = '\0';
  }
}

int SetCudaStartError(cudaError_t error, std::string_view operation,
                      char* error_buffer,
                      std::size_t error_buffer_capacity) noexcept {
  try {
    SetErrorMessage(fmt::format("{}: {}", operation, cudaGetErrorString(error)),
                    error_buffer, error_buffer_capacity);
  } catch (...) {
    SetErrorMessage(operation, error_buffer, error_buffer_capacity);
  }
  return 1;
}

int MW_PROCESSOR_CALL OnStart(const MwProcessorContext* context,
                              const char* /*config*/, char* error_buffer,
                              std::size_t error_buffer_capacity,
                              void* user_data) noexcept {
  auto* const state = static_cast<ProcessorState*>(user_data);
  if (context == nullptr || state == nullptr) {
    SetErrorMessage("processor context or user data is null", error_buffer,
                    error_buffer_capacity);
    return 1;
  }

  if (state->mode == CopyMode::kIdentity) {
    if (context->input_count != 1) {
      SetErrorMessage("identity mode requires exactly one input", error_buffer,
                      error_buffer_capacity);
      return 1;
    }
    const MwVideoSourceInfo& input = context->inputs[0].video;
    if (input.width != context->video_output.width ||
        input.height != context->video_output.height) {
      SetErrorMessage(
          "identity mode requires output dimensions to match the input",
          error_buffer, error_buffer_capacity);
      return 1;
    }
  } else {
    if (context->input_count != 2) {
      SetErrorMessage("vstack mode requires exactly two inputs", error_buffer,
                      error_buffer_capacity);
      return 1;
    }
    const MwVideoSourceInfo& first = context->inputs[0].video;
    const MwVideoSourceInfo& second = context->inputs[1].video;
    if (first.width != second.width ||
        context->video_output.width != first.width ||
        context->video_output.height != first.height + second.height) {
      SetErrorMessage(
          "vstack mode requires equal input widths and an output sized to "
          "input width by the sum of input heights",
          error_buffer, error_buffer_capacity);
      return 1;
    }
  }

  const cudaError_t set_device_result = cudaSetDevice(context->device_id);
  if (set_device_result != cudaSuccess) {
    return SetCudaStartError(set_device_result, "cudaSetDevice failed",
                             error_buffer, error_buffer_capacity);
  }
  const cudaError_t create_stream_result =
      cudaStreamCreateWithFlags(&state->stream, cudaStreamNonBlocking);
  if (create_stream_result != cudaSuccess) {
    return SetCudaStartError(create_stream_result,
                             "cudaStreamCreateWithFlags failed", error_buffer,
                             error_buffer_capacity);
  }
  return 0;
}

void RecordCudaError(ProcessorState* state, cudaError_t error) noexcept {
  if (error == cudaSuccess) {
    return;
  }
  int expected = cudaSuccess;
  state->callback_error.compare_exchange_strong(
      expected, static_cast<int>(error), std::memory_order_relaxed);
}

void CopyPlane(const std::uint8_t* source, std::size_t source_pitch,
               std::uint8_t* destination, std::size_t destination_pitch,
               std::size_t width_bytes, std::size_t height,
               ProcessorState* state) noexcept {
  if (state->callback_error.load(std::memory_order_relaxed) != cudaSuccess) {
    return;
  }
  RecordCudaError(state,
                  cudaMemcpy2DAsync(destination, destination_pitch, source,
                                    source_pitch, width_bytes, height,
                                    cudaMemcpyDeviceToDevice, state->stream));
}

void MW_PROCESSOR_CALL OnVideo(const MwGpuNv12FrameView* inputs,
                               std::size_t input_count,
                               MwGpuNv12FrameView* output,
                               void* user_data) noexcept {
  auto* const state = static_cast<ProcessorState*>(user_data);
  if (state != nullptr) {
    state->video_callback_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (inputs == nullptr || output == nullptr || state == nullptr ||
      input_count == 0 || state->stream == nullptr ||
      state->callback_error.load(std::memory_order_relaxed) != cudaSuccess) {
    return;
  }

  const MwGpuNv12FrameView& first = inputs[0];
  CopyPlane(first.y, first.y_pitch_bytes, output->y, output->y_pitch_bytes,
            static_cast<std::size_t>(first.width),
            static_cast<std::size_t>(first.height), state);
  CopyPlane(first.uv, first.uv_pitch_bytes, output->uv, output->uv_pitch_bytes,
            static_cast<std::size_t>(first.width),
            static_cast<std::size_t>(first.height / 2), state);

  if (state->mode == CopyMode::kVerticalStack) {
    const MwGpuNv12FrameView& second = inputs[1];
    CopyPlane(second.y, second.y_pitch_bytes,
              output->y + static_cast<std::size_t>(first.height) *
                              output->y_pitch_bytes,
              output->y_pitch_bytes, static_cast<std::size_t>(second.width),
              static_cast<std::size_t>(second.height), state);
    CopyPlane(second.uv, second.uv_pitch_bytes,
              output->uv + static_cast<std::size_t>(first.height / 2) *
                               output->uv_pitch_bytes,
              output->uv_pitch_bytes, static_cast<std::size_t>(second.width),
              static_cast<std::size_t>(second.height / 2), state);
  }

  if (state->callback_error.load(std::memory_order_relaxed) == cudaSuccess) {
    RecordCudaError(state, cudaStreamSynchronize(state->stream));
  }
}

void MW_PROCESSOR_CALL OnAudio(const MwAudioFrameView* inputs,
                               std::size_t input_count,
                               MwAudioFrameView* output,
                               void* user_data) noexcept {
  auto* const state = static_cast<ProcessorState*>(user_data);
  if (state != nullptr) {
    state->audio_callback_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (inputs == nullptr || input_count == 0 || inputs[0].samples == nullptr ||
      output == nullptr || output->samples == nullptr) {
    return;
  }

  const std::size_t sample_count =
      output->frame_count * static_cast<std::size_t>(output->channel_count);
  std::copy_n(inputs[0].samples, sample_count, output->samples);
}

void MW_PROCESSOR_CALL OnStop(void* user_data) noexcept {
  auto* const state = static_cast<ProcessorState*>(user_data);
  if (state == nullptr || state->stream == nullptr) {
    return;
  }
  RecordCudaError(state, cudaStreamDestroy(state->stream));
  state->stream = nullptr;
}

bool ParsePositiveInt(std::string_view text, int* value) {
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, *value);
  return error == std::errc{} && position == end && *value > 0;
}

bool ParseNonNegativeInt(std::string_view text, int* value) {
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, *value);
  return error == std::errc{} && position == end && *value >= 0;
}

bool ParseVideoCodec(std::string_view text, mw::streamer::VideoCodec* codec) {
  if (text == "h264") {
    *codec = mw::streamer::VideoCodec::kH264;
  } else if (text == "h265") {
    *codec = mw::streamer::VideoCodec::kH265;
  } else {
    return false;
  }
  return true;
}

bool ParseInputProtocol(std::string_view text,
                        mw::streamer::InputConfig* input) {
  if (text == "file") {
    input->protocol = mw::streamer::InputProtocol::kFile;
  } else if (text == "rtsp" || text == "rtsp-tcp") {
    input->protocol = mw::streamer::InputProtocol::kRtsp;
    input->rtsp_transport = mw::streamer::RtspTransport::kTcp;
  } else if (text == "rtsp-udp") {
    input->protocol = mw::streamer::InputProtocol::kRtsp;
    input->rtsp_transport = mw::streamer::RtspTransport::kUdp;
  } else if (text == "rtmp") {
    input->protocol = mw::streamer::InputProtocol::kRtmp;
  } else if (text == "srt") {
    input->protocol = mw::streamer::InputProtocol::kSrt;
  } else if (text == "hls-live") {
    input->protocol = mw::streamer::InputProtocol::kHls;
    input->hls_mode = mw::streamer::HlsMode::kLive;
  } else {
    return false;
  }
  return true;
}

bool ParseOutputProtocol(std::string_view text,
                         mw::streamer::OutputConfig* output) {
  if (text == "file") {
    output->protocol = mw::streamer::OutputProtocol::kFile;
  } else if (text == "rtsp" || text == "rtsp-tcp") {
    output->protocol = mw::streamer::OutputProtocol::kRtsp;
    output->rtsp_transport = mw::streamer::RtspTransport::kTcp;
  } else if (text == "rtsp-udp") {
    output->protocol = mw::streamer::OutputProtocol::kRtsp;
    output->rtsp_transport = mw::streamer::RtspTransport::kUdp;
  } else if (text == "rtmp") {
    output->protocol = mw::streamer::OutputProtocol::kRtmp;
  } else if (text == "srt") {
    output->protocol = mw::streamer::OutputProtocol::kSrt;
  } else {
    return false;
  }
  return true;
}

void ConfigurePipeline(mw::streamer::PipelineConfig* config,
                       ProcessorState* processor_state,
                       mw::streamer::VideoCodec codec, int width, int height,
                       int frame_rate, std::string_view standby_image_path) {
  config->id = "cuda-processor-example";
  config->video_encoder.codec = codec;
  config->video_encoder.width = width;
  config->video_encoder.height = height;
  config->video_encoder.frame_rate = {frame_rate, 1};
  config->video_encoder.bit_rate = 1'000'000;
  config->video_encoder.maximum_bit_rate = 1'000'000;
  config->video_encoder.vbv_buffer_size = 2'000'000;
  config->video_encoder.gop_size = frame_rate;
  config->standby_image_path = std::string(standby_image_path);
  config->processor_callbacks.on_start = OnStart;
  config->processor_callbacks.on_video = OnVideo;
  config->processor_callbacks.on_audio = OnAudio;
  config->processor_callbacks.on_stop = OnStop;
  config->processor_callbacks.user_data = processor_state;
  config->callbacks.on_state_changed = [](mw::streamer::PipelineState state,
                                          const mw::streamer::Status& status) {
    fmt::print("pipeline state={} status={} message={}\n",
               static_cast<int>(state), static_cast<int>(status.code()),
               status.message());
  };
  config->callbacks.on_endpoint_event =
      [](const mw::streamer::EndpointEvent& event) {
        fmt::print("endpoint={} event={} status={} message={}\n",
                   event.endpoint_id, static_cast<int>(event.type),
                   static_cast<int>(event.status.code()),
                   event.status.message());
      };
}

int RunPipeline(mw::streamer::PipelineConfig config,
                ProcessorState* processor_state, int run_seconds) {
  mw::streamer::Pipeline pipeline(std::move(config));
  const mw::streamer::Status start_status = pipeline.Start();
  mw::streamer::Status terminal_status = start_status;
  if (!start_status.ok()) {
    fmt::print(stderr, "pipeline start failed: {}\n", start_status.message());
  } else {
    if (run_seconds == 0) {
      terminal_status = pipeline.Wait();
    } else {
      std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
      terminal_status = pipeline.Stop();
    }
  }

  const std::uint64_t video_callback_count =
      processor_state->video_callback_count.load(std::memory_order_relaxed);
  const std::uint64_t audio_callback_count =
      processor_state->audio_callback_count.load(std::memory_order_relaxed);
  fmt::print("video callbacks={} audio callbacks={}\n", video_callback_count,
             audio_callback_count);

  if (!start_status.ok()) {
    return 1;
  }
  if (!terminal_status.ok()) {
    fmt::print(stderr, "pipeline failed: {}\n", terminal_status.message());
    return 1;
  }

  const auto callback_error = static_cast<cudaError_t>(
      processor_state->callback_error.load(std::memory_order_relaxed));
  if (callback_error != cudaSuccess) {
    fmt::print(stderr, "CUDA processor callback failed: {}\n",
               cudaGetErrorString(callback_error));
    return 1;
  }
  fmt::print("pipeline finished successfully\n");
  return 0;
}

void PrintUsage(std::string_view program) {
  fmt::print(stderr,
             "usage:\n"
             "  {} identity <output.mp4> <standby.jpg|png> <width> <height> "
             "<fps> <input.mp4> [h264|h265]\n"
             "  {} vstack <output.mp4> <standby.jpg|png> <width> <height> "
             "<fps> <input-1.mp4> <input-2.mp4> [h264|h265]\n"
             "  {} stream-identity "
             "<file|rtsp[-tcp|-udp]|rtmp|srt|hls-live> <input-url> "
             "<standby.jpg|png> <width> <height> <fps> "
             "<run-seconds,0=until-eof> <h264|h265> "
             "<file|rtsp[-tcp|-udp]|rtmp|srt> <output-url> "
             "[<output-protocol> <output-url>]...\n",
             program, program, program);
}

int RunStreamIdentity(int argc, char** argv) {
  if (argc < 12 || (argc - 10) % 2 != 0) {
    PrintUsage(argv[0]);
    return 2;
  }

  int width = 0;
  int height = 0;
  int frame_rate = 0;
  int run_seconds = 0;
  mw::streamer::VideoCodec codec;
  mw::streamer::InputConfig input;
  input.id = "input-0";
  input.url = argv[3];
  if (!ParseInputProtocol(argv[2], &input) ||
      !ParsePositiveInt(argv[5], &width) ||
      !ParsePositiveInt(argv[6], &height) ||
      !ParsePositiveInt(argv[7], &frame_rate) ||
      !ParseNonNegativeInt(argv[8], &run_seconds) ||
      !ParseVideoCodec(argv[9], &codec)) {
    PrintUsage(argv[0]);
    return 2;
  }

  ProcessorState processor_state;
  mw::streamer::PipelineConfig config;
  config.inputs.push_back(std::move(input));
  for (int argument_index = 10; argument_index < argc; argument_index += 2) {
    mw::streamer::OutputConfig output;
    output.id = fmt::format("output-{}", config.outputs.size());
    output.url = argv[argument_index + 1];
    if (!ParseOutputProtocol(argv[argument_index], &output)) {
      PrintUsage(argv[0]);
      return 2;
    }
    config.outputs.push_back(std::move(output));
  }
  ConfigurePipeline(&config, &processor_state, codec, width, height, frame_rate,
                    argv[4]);
  return RunPipeline(std::move(config), &processor_state, run_seconds);
}

int RunLegacyMode(int argc, char** argv) {
  if (argc < 8 || argc > 10) {
    PrintUsage(argv[0]);
    return 2;
  }

  ProcessorState processor_state;
  const std::string_view mode(argv[1]);
  if (mode == "identity" && (argc == 8 || argc == 9)) {
    processor_state.mode = CopyMode::kIdentity;
  } else if (mode == "vstack" && (argc == 9 || argc == 10)) {
    processor_state.mode = CopyMode::kVerticalStack;
  } else {
    PrintUsage(argv[0]);
    return 2;
  }

  int width = 0;
  int height = 0;
  int frame_rate = 0;
  if (!ParsePositiveInt(argv[4], &width) ||
      !ParsePositiveInt(argv[5], &height) ||
      !ParsePositiveInt(argv[6], &frame_rate)) {
    PrintUsage(argv[0]);
    return 2;
  }

  mw::streamer::VideoCodec codec = mw::streamer::VideoCodec::kH264;
  const int codec_index = processor_state.mode == CopyMode::kIdentity ? 8 : 9;
  if (argc > codec_index && !ParseVideoCodec(argv[codec_index], &codec)) {
    PrintUsage(argv[0]);
    return 2;
  }

  mw::streamer::PipelineConfig config;
  const int input_count = processor_state.mode == CopyMode::kIdentity ? 1 : 2;
  for (int input_index = 0; input_index < input_count; ++input_index) {
    mw::streamer::InputConfig input;
    input.id = fmt::format("input-{}", input_index);
    input.protocol = mw::streamer::InputProtocol::kFile;
    input.url = argv[7 + input_index];
    config.inputs.push_back(std::move(input));
  }

  mw::streamer::OutputConfig output;
  output.id = "recording";
  output.protocol = mw::streamer::OutputProtocol::kFile;
  output.url = argv[2];
  config.outputs.push_back(std::move(output));
  ConfigurePipeline(&config, &processor_state, codec, width, height, frame_rate,
                    argv[3]);
  return RunPipeline(std::move(config), &processor_state, 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "stream-identity") {
    return RunStreamIdentity(argc, argv);
  }
  return RunLegacyMode(argc, argv);
}

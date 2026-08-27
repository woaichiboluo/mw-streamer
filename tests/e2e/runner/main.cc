#include <cuda.h>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/init/init.h"
#include "mw/output/output_sink.h"
#include "mw/pipeline/file_pipeline.h"
#include "mw/pipeline/remux_pipeline.h"
#include "mw/pipeline/streaming_pipeline.h"
#include "mw/processor/processor.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::output::OutputSink;
using mw::streamer::pipeline::FilePipeline;
using mw::streamer::pipeline::FilePipelineStatus;
using mw::streamer::pipeline::LocalFilePipelineConfig;
using mw::streamer::pipeline::RemuxPipeline;
using mw::streamer::pipeline::RemuxPipelineConfig;
using mw::streamer::pipeline::RemuxPipelineStatus;
using mw::streamer::pipeline::StreamingPipeline;
using mw::streamer::pipeline::StreamingPipelineConfig;
using mw::streamer::pipeline::StreamingPipelineStatus;

std::atomic_bool g_stop_requested = false;

void HandleSignal(int) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

const char* ToString(StreamingPipelineStatus status) {
  switch (status) {
    case StreamingPipelineStatus::kIdle:
      return "idle";
    case StreamingPipelineStatus::kStarting:
      return "starting";
    case StreamingPipelineStatus::kRunning:
      return "running";
    case StreamingPipelineStatus::kFailed:
      return "failed";
    case StreamingPipelineStatus::kStopped:
      return "stopped";
  }
  return "unknown";
}

const char* ToString(FilePipelineStatus status) {
  switch (status) {
    case FilePipelineStatus::kIdle:
      return "idle";
    case FilePipelineStatus::kStarting:
      return "starting";
    case FilePipelineStatus::kRunning:
      return "running";
    case FilePipelineStatus::kFailed:
      return "failed";
    case FilePipelineStatus::kStopped:
      return "stopped";
  }
  return "unknown";
}

const char* ToString(RemuxPipelineStatus status) {
  switch (status) {
    case RemuxPipelineStatus::kIdle:
      return "idle";
    case RemuxPipelineStatus::kStarting:
      return "starting";
    case RemuxPipelineStatus::kRunning:
      return "running";
    case RemuxPipelineStatus::kFailed:
      return "failed";
    case RemuxPipelineStatus::kStopped:
      return "stopped";
  }
  return "unknown";
}

class EventWriter final {
 public:
  explicit EventWriter(const std::string& path)
      : started_at_(std::chrono::steady_clock::now()), output_(path) {
    if (!output_) {
      throw std::runtime_error("无法打开事件输出文件: " + path);
    }
  }

  void Write(
      const std::string& event,
      const std::vector<std::pair<std::string, std::string>>& fields = {}) {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_)
            .count();
    auto line = fmt::format("ts_ms={} event={}", elapsed_ms, event);
    for (const auto& [key, value] : fields) {
      fmt::format_to(std::back_inserter(line), " {}={}", key, value);
    }
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    output_.write(line.data(), static_cast<std::streamsize>(line.size()));
    output_.flush();
  }

 private:
  std::chrono::steady_clock::time_point started_at_;
  std::mutex mutex_;
  std::ofstream output_;
};

enum class PipelineKind {
  kStreaming,
  kRemux,
  kFile,
};

struct Arguments {
  PipelineKind pipeline = PipelineKind::kStreaming;
  std::string input;
  std::vector<std::string> outputs;
  std::vector<std::string> input_outputs;
  std::string events_path;
  std::chrono::milliseconds cache_duration{1000};
  std::chrono::milliseconds duration{10000};
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::uint32_t frame_rate_num = 0;
  std::uint32_t frame_rate_den = 1;
  MwStreamerCodec video_codec = kMwStreamerCodecUnknown;
  std::chrono::milliseconds video_jitter_min{0};
  std::chrono::milliseconds video_jitter_max{0};
  bool passthrough_video = false;
  bool software_video = false;
  bool standby = false;
  bool local_sink = false;
};

std::string RequireValue(int argc, char* argv[], int& index) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("缺少参数值: ") + argv[index]);
  }
  return argv[++index];
}

std::chrono::milliseconds ParseMilliseconds(const std::string& value,
                                            const char* option,
                                            std::int64_t minimum) {
  std::size_t parsed = 0;
  const auto number = std::stoll(value, &parsed);
  if (parsed != value.size() || number < minimum) {
    throw std::invalid_argument(
        fmt::format("{}必须为不小于{}的整数", option, minimum));
  }
  return std::chrono::milliseconds(number);
}

std::uint32_t ParseUnsigned(const std::string& value, const char* option) {
  std::size_t parsed = 0;
  const auto number = std::stoull(value, &parsed);
  if (parsed != value.size() ||
      number > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(fmt::format("{}必须是有效的非负整数", option));
  }
  return static_cast<std::uint32_t>(number);
}

MwStreamerCodec ParseVideoCodec(const std::string& value) {
  if (value == "none") {
    return kMwStreamerCodecUnknown;
  }
  if (value == "h264") {
    return kMwStreamerCodecH264;
  }
  if (value == "h265") {
    return kMwStreamerCodecH265;
  }
  throw std::invalid_argument("--video-codec必须是none、h264或h265");
}

PipelineKind ParsePipelineKind(const std::string& value) {
  if (value == "streaming") {
    return PipelineKind::kStreaming;
  }
  if (value == "remux") {
    return PipelineKind::kRemux;
  }
  if (value == "file") {
    return PipelineKind::kFile;
  }
  throw std::invalid_argument("--pipeline必须是streaming、remux或file");
}

Arguments ParseArguments(int argc, char* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--pipeline") {
      arguments.pipeline = ParsePipelineKind(RequireValue(argc, argv, index));
    } else if (option == "--input") {
      arguments.input = RequireValue(argc, argv, index);
    } else if (option == "--output") {
      arguments.outputs.push_back(RequireValue(argc, argv, index));
    } else if (option == "--input-output") {
      arguments.input_outputs.push_back(RequireValue(argc, argv, index));
    } else if (option == "--events") {
      arguments.events_path = RequireValue(argc, argv, index);
    } else if (option == "--cache-ms") {
      arguments.cache_duration =
          ParseMilliseconds(RequireValue(argc, argv, index), "--cache-ms", 0);
    } else if (option == "--duration-ms") {
      arguments.duration = ParseMilliseconds(RequireValue(argc, argv, index),
                                             "--duration-ms", 1);
    } else if (option == "--output-width") {
      arguments.output_width =
          ParseUnsigned(RequireValue(argc, argv, index), "--output-width");
    } else if (option == "--output-height") {
      arguments.output_height =
          ParseUnsigned(RequireValue(argc, argv, index), "--output-height");
    } else if (option == "--frame-rate-num") {
      arguments.frame_rate_num =
          ParseUnsigned(RequireValue(argc, argv, index), "--frame-rate-num");
    } else if (option == "--frame-rate-den") {
      arguments.frame_rate_den =
          ParseUnsigned(RequireValue(argc, argv, index), "--frame-rate-den");
    } else if (option == "--video-codec") {
      arguments.video_codec = ParseVideoCodec(RequireValue(argc, argv, index));
    } else if (option == "--video-jitter-min-ms") {
      arguments.video_jitter_min = ParseMilliseconds(
          RequireValue(argc, argv, index), "--video-jitter-min-ms", 0);
    } else if (option == "--video-jitter-max-ms") {
      arguments.video_jitter_max = ParseMilliseconds(
          RequireValue(argc, argv, index), "--video-jitter-max-ms", 0);
    } else if (option == "--passthrough-video") {
      arguments.passthrough_video = true;
    } else if (option == "--software-video") {
      arguments.software_video = true;
    } else if (option == "--standby") {
      arguments.standby = true;
    } else if (option == "--local-sink") {
      arguments.local_sink = true;
    } else {
      throw std::invalid_argument("未知参数: " + option);
    }
  }

  if (arguments.input.empty()) {
    throw std::invalid_argument("--input不能为空");
  }
  if (arguments.pipeline == PipelineKind::kRemux && arguments.outputs.empty()) {
    throw std::invalid_argument("RemuxPipeline的--output至少需要一个");
  }
  if (arguments.events_path.empty()) {
    throw std::invalid_argument("--events不能为空");
  }
  if (arguments.pipeline == PipelineKind::kRemux &&
      !arguments.input_outputs.empty()) {
    throw std::invalid_argument("RemuxPipeline不支持--input-output");
  }
  if (arguments.pipeline == PipelineKind::kFile &&
      (!arguments.outputs.empty() || !arguments.input_outputs.empty())) {
    throw std::invalid_argument("FilePipeline不支持输出目标");
  }
  if (arguments.pipeline != PipelineKind::kStreaming && arguments.local_sink) {
    throw std::invalid_argument("只有StreamingPipeline支持--local-sink");
  }
  if ((arguments.output_width == 0) != (arguments.output_height == 0)) {
    throw std::invalid_argument("视频输出宽高必须同时为0或同时大于0");
  }
  if (arguments.frame_rate_den == 0 ||
      (arguments.output_width == 0 && arguments.frame_rate_num != 0)) {
    throw std::invalid_argument("视频帧率参数无效");
  }
  if ((arguments.output_width == 0) !=
      (arguments.video_codec == kMwStreamerCodecUnknown)) {
    throw std::invalid_argument("视频输出尺寸与编码格式不匹配");
  }
  if (arguments.video_jitter_min > arguments.video_jitter_max) {
    throw std::invalid_argument("视频抖动最小值不能大于最大值");
  }
  if (arguments.video_jitter_max > 0ms && !arguments.passthrough_video) {
    throw std::invalid_argument("视频抖动测试必须启用视频透传");
  }
  if (arguments.passthrough_video && arguments.output_width == 0) {
    throw std::invalid_argument("纯音频输入不能启用视频透传");
  }
  return arguments;
}

struct ProcessorObserver {
  EventWriter* events = nullptr;
  std::atomic_uint64_t timeline_reset_count{0};
  std::chrono::milliseconds video_jitter_min{0};
  std::chrono::milliseconds video_jitter_max{0};
  MwStreamerExecutionContext execution{};
  CUcontext cuda_context = nullptr;
  CUstream cuda_stream = nullptr;
  std::uint64_t video_frame_count = 0;
  std::uint64_t video_jitter_count = 0;
};

struct LocalSinkObserver {
  EventWriter* events = nullptr;
  std::atomic_uint64_t starts{0};
  std::atomic_uint64_t stops{0};
  std::atomic_uint64_t video_frames{0};
  std::atomic_uint64_t audio_frames{0};
  std::atomic_uint64_t invalid_frames{0};
};

class ObservingOutputSink final : public OutputSink {
 public:
  explicit ObservingOutputSink(LocalSinkObserver& observer)
      : observer_(observer) {}

  void Start() override {
    observer_.starts.fetch_add(1, std::memory_order_relaxed);
    observer_.events->Write("local_sink_started");
  }

  void WriteAudio(mw::streamer::ffmpeg::Frame frame) override {
    if (!frame.get() || !frame->data[0] || frame->sample_rate <= 0 ||
        frame->ch_layout.nb_channels <= 0 || frame->nb_samples <= 0 ||
        frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      observer_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    observer_.audio_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void WriteVideo(mw::streamer::ffmpeg::Frame frame) override {
    if (!frame.get() || !frame->data[0] || frame->width <= 0 ||
        frame->height <= 0 || frame->format == AV_PIX_FMT_NONE ||
        frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      observer_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    observer_.video_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void Stop() noexcept override {
    observer_.stops.fetch_add(1, std::memory_order_relaxed);
    observer_.events->Write("local_sink_stopped");
  }

 private:
  LocalSinkObserver& observer_;
};

struct FileProcessorObserver {
  EventWriter* events = nullptr;
  std::atomic_uint64_t video_frames{0};
  std::atomic_uint64_t audio_frames{0};
  std::atomic_uint64_t audio_samples{0};
  std::atomic_uint64_t end_of_input_count{0};
  std::atomic_uint64_t stop_count{0};
};

MwStreamerProcessorStartResult OnFileProcessorStart(
    const MwStreamerFileProcessorStartRequest* request, void* user_context) {
  auto* observer = static_cast<FileProcessorObserver*>(user_context);
  if (!observer || !observer->events || !request || !request->source_info ||
      !request->config || !request->execution) {
    return kMwStreamerProcessorStartFailed;
  }
  observer->events->Write(
      "processor_started",
      {{"has_audio", request->source_info->has_audio ? "1" : "0"},
       {"has_video", request->source_info->has_video ? "1" : "0"},
       {"source_width", std::to_string(request->source_info->video.width)},
       {"source_height", std::to_string(request->source_info->video.height)},
       {"execution", request->execution->type == kMwStreamerExecutionCuda
                         ? "cuda"
                         : "cpu"}});
  return kMwStreamerProcessorStartSuccess;
}

void ProcessFileVideo(const MwStreamerVideoFrameView* input,
                      void* user_context) {
  auto* observer = static_cast<FileProcessorObserver*>(user_context);
  if (!observer || !input || input->buffer.width == 0 ||
      input->buffer.height == 0 ||
      input->buffer.memory_type != kMwStreamerMemoryHost) {
    throw std::invalid_argument("E2E File Processor视频帧无效");
  }
  observer->video_frames.fetch_add(1, std::memory_order_relaxed);
}

void ProcessFileAudio(const MwStreamerAudioFrameView* input,
                      void* user_context) {
  auto* observer = static_cast<FileProcessorObserver*>(user_context);
  if (!observer || !input || !input->data || input->sample_rate != 48000 ||
      input->channel_count == 0 || input->samples_per_channel == 0) {
    throw std::invalid_argument("E2E File Processor音频帧无效");
  }
  observer->audio_frames.fetch_add(1, std::memory_order_relaxed);
  observer->audio_samples.fetch_add(input->samples_per_channel,
                                    std::memory_order_relaxed);
}

void OnFileProcessorBoundary(MwStreamerProcessorBoundaryReason reason,
                             void* user_context) {
  auto* observer = static_cast<FileProcessorObserver*>(user_context);
  if (!observer || !observer->events ||
      reason != kMwStreamerProcessorEndOfInput) {
    throw std::invalid_argument("E2E File Processor收到未知边界");
  }
  const auto count =
      observer->end_of_input_count.fetch_add(1, std::memory_order_acq_rel) + 1;
  observer->events->Write(
      "processor_boundary",
      {{"reason", "end_of_input"}, {"count", std::to_string(count)}});
}

void OnFileProcessorStop(void* user_context) {
  auto* observer = static_cast<FileProcessorObserver*>(user_context);
  if (!observer || !observer->events) {
    return;
  }
  const auto count =
      observer->stop_count.fetch_add(1, std::memory_order_acq_rel) + 1;
  observer->events->Write("processor_stopped",
                          {{"count", std::to_string(count)}});
}

MwStreamerProcessorStartResult OnProcessorStart(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context) {
  auto* observer = static_cast<ProcessorObserver*>(user_context);
  if (!observer || !observer->events || !request || !request->source_info ||
      !request->config || !request->execution) {
    return kMwStreamerProcessorStartFailed;
  }
  observer->execution = *request->execution;
  if (observer->execution.type == kMwStreamerExecutionCuda) {
    CUdevice cuda_device = 0;
    if (cuInit(0) != CUDA_SUCCESS ||
        cuDeviceGet(&cuda_device, 0) != CUDA_SUCCESS ||
        cuCtxCreate(&observer->cuda_context, CU_CTX_SCHED_AUTO, cuda_device) !=
            CUDA_SUCCESS) {
      return kMwStreamerProcessorStartFailed;
    }

    CUcontext popped_context = nullptr;
    if (cuStreamCreate(&observer->cuda_stream, CU_STREAM_NON_BLOCKING) !=
            CUDA_SUCCESS ||
        cuCtxPopCurrent(&popped_context) != CUDA_SUCCESS ||
        popped_context != observer->cuda_context) {
      if (observer->cuda_context) {
        cuCtxDestroy(observer->cuda_context);
        observer->cuda_context = nullptr;
      }
      observer->cuda_stream = nullptr;
      return kMwStreamerProcessorStartFailed;
    }
  }

  observer->events->Write(
      "processor_started",
      {{"has_audio", request->source_info->has_audio ? "1" : "0"},
       {"has_video", request->source_info->has_video ? "1" : "0"},
       {"source_width", std::to_string(request->source_info->video.width)},
       {"source_height", std::to_string(request->source_info->video.height)},
       {"output_width", std::to_string(request->config->output_width)},
       {"output_height", std::to_string(request->config->output_height)},
       {"execution", request->execution->type == kMwStreamerExecutionCuda
                         ? "cuda"
                         : "cpu"}});
  return kMwStreamerProcessorStartSuccess;
}

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* name = nullptr;
  cuGetErrorName(result, &name);
  throw std::runtime_error(
      fmt::format("{}失败: {}", operation, name ? name : "CUDA_ERROR_UNKNOWN"));
}

void CopyHostVideo(const MwStreamerVideoBufferView& input,
                   MwStreamerVideoBufferView* output) {
  for (std::uint32_t plane = 0; plane < input.storage.linear.plane_count;
       ++plane) {
    const auto& source = input.storage.linear.planes[plane];
    const auto& destination = output->storage.linear.planes[plane];
    for (std::uint32_t row = 0; row < source.row_count; ++row) {
      const auto* source_row =
          reinterpret_cast<const std::uint8_t*>(source.address) +
          static_cast<std::ptrdiff_t>(row) * source.stride_bytes;
      auto* destination_row =
          reinterpret_cast<std::uint8_t*>(destination.address) +
          static_cast<std::ptrdiff_t>(row) * destination.stride_bytes;
      std::memcpy(destination_row, source_row, source.row_bytes);
    }
  }
}

void CopyCudaVideo(const MwStreamerVideoBufferView& input,
                   MwStreamerVideoBufferView* output, CUstream stream) {
  for (std::uint32_t plane = 0; plane < input.storage.linear.plane_count;
       ++plane) {
    const auto& source = input.storage.linear.planes[plane];
    const auto& destination = output->storage.linear.planes[plane];
    if (source.stride_bytes <= 0 || destination.stride_bytes <= 0) {
      throw std::invalid_argument("E2E CUDA视频平面stride无效");
    }
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_UNIFIED;
    copy.srcDevice = static_cast<CUdeviceptr>(source.address);
    copy.srcPitch = static_cast<std::size_t>(source.stride_bytes);
    copy.dstMemoryType = CU_MEMORYTYPE_UNIFIED;
    copy.dstDevice = static_cast<CUdeviceptr>(destination.address);
    copy.dstPitch = static_cast<std::size_t>(destination.stride_bytes);
    copy.WidthInBytes = source.row_bytes;
    copy.Height = source.row_count;
    ThrowIfCudaError(cuMemcpy2DAsync(&copy, stream), "复制E2E CUDA视频帧");
  }
}

void ProcessVideo(const MwStreamerStreamingVideoProcessRequest* request,
                  void* user_context) {
  auto* observer = static_cast<ProcessorObserver*>(user_context);
  if (!observer || !request || !request->input || !request->output) {
    throw std::invalid_argument("E2E视频透传回调参数无效");
  }
  const auto& input = request->input->buffer;
  auto* output = request->output;
  if (input.memory_type != output->memory_type ||
      input.storage_type != kMwStreamerVideoStorageLinear ||
      output->storage_type != kMwStreamerVideoStorageLinear ||
      input.pixel_format != output->pixel_format ||
      input.width != output->width || input.height != output->height ||
      input.storage.linear.plane_count != output->storage.linear.plane_count) {
    throw std::invalid_argument("E2E视频透传输入输出格式不匹配");
  }
  for (std::uint32_t plane = 0; plane < input.storage.linear.plane_count;
       ++plane) {
    const auto& source = input.storage.linear.planes[plane];
    const auto& destination = output->storage.linear.planes[plane];
    if (source.row_bytes != destination.row_bytes ||
        source.row_count != destination.row_count) {
      throw std::invalid_argument("E2E视频透传平面布局不匹配");
    }
  }

  ++observer->video_frame_count;
  if (observer->video_jitter_max > 0ms &&
      observer->video_frame_count % 30 == 0) {
    const auto delay = observer->video_jitter_count++ % 2 == 0
                           ? observer->video_jitter_min
                           : observer->video_jitter_max;
    std::this_thread::sleep_for(delay);
    observer->events->Write(
        "video_jitter",
        {{"delay_ms", std::to_string(delay.count())},
         {"frame", std::to_string(observer->video_frame_count)}});
  }

  if (input.memory_type == kMwStreamerMemoryHost) {
    CopyHostVideo(input, output);
    return;
  }
  if (input.memory_type != kMwStreamerMemoryCuda ||
      observer->execution.type != kMwStreamerExecutionCuda ||
      !observer->cuda_context || !observer->cuda_stream) {
    throw std::invalid_argument("E2E视频透传收到未知执行上下文");
  }
  CUcontext popped_context = nullptr;
  ThrowIfCudaError(cuCtxPushCurrent(observer->cuda_context),
                   "设置E2E用户CUDA上下文");
  try {
    CopyCudaVideo(input, output, observer->cuda_stream);
    ThrowIfCudaError(cuStreamSynchronize(observer->cuda_stream),
                     "等待E2E用户CUDA复制完成");
  } catch (...) {
    cuStreamSynchronize(observer->cuda_stream);
    cuCtxPopCurrent(&popped_context);
    throw;
  }
  ThrowIfCudaError(cuCtxPopCurrent(&popped_context), "恢复E2E用户CUDA上下文");
}

void OnProcessorBoundary(MwStreamerProcessorBoundaryReason reason,
                         void* user_context) {
  auto* observer = static_cast<ProcessorObserver*>(user_context);
  if (!observer || !observer->events) {
    throw std::invalid_argument("E2E Processor边界回调参数无效");
  }

  const char* reason_name = nullptr;
  std::uint64_t reset_count = observer->timeline_reset_count.load();
  switch (reason) {
    case kMwStreamerProcessorTimelineReset:
      reason_name = "timeline_reset";
      reset_count = observer->timeline_reset_count.fetch_add(
                        1, std::memory_order_acq_rel) +
                    1;
      break;
    case kMwStreamerProcessorEndOfInput:
      reason_name = "end_of_input";
      break;
    default:
      throw std::invalid_argument("E2E Processor收到未知边界");
  }
  observer->events->Write(
      "processor_boundary",
      {{"reason", reason_name},
       {"timeline_reset_count", std::to_string(reset_count)}});
}

void OnProcessorStop(void* user_context) {
  auto* observer = static_cast<ProcessorObserver*>(user_context);
  if (!observer) {
    return;
  }
  if (observer->cuda_context) {
    cuCtxDestroy(observer->cuda_context);
    observer->cuda_context = nullptr;
    observer->cuda_stream = nullptr;
  }
  if (observer->events) {
    observer->events->Write("processor_stopped");
  }
}

int RunStreaming(const Arguments& arguments, EventWriter& events) {
  ProcessorObserver observer;
  observer.events = &events;
  observer.video_jitter_min = arguments.video_jitter_min;
  observer.video_jitter_max = arguments.video_jitter_max;
  LocalSinkObserver local_sink_observer;
  local_sink_observer.events = &events;

  StreamingPipelineConfig config;
  config.input_url = arguments.input;
  config.input_targets = arguments.input_outputs;
  config.output_targets = arguments.outputs;
  config.cache_duration = arguments.cache_duration;
  config.processor.output_width = arguments.output_width;
  config.processor.output_height = arguments.output_height;
  config.video_encoder.frame_rate = {
      static_cast<std::int32_t>(arguments.frame_rate_num),
      static_cast<std::int32_t>(arguments.frame_rate_den),
  };
  config.video_encoder.codec = arguments.video_codec;
  if (arguments.software_video) {
    config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  }
  config.standby.enabled = arguments.standby;

  StreamingPipeline pipeline(std::move(config));
  if (arguments.local_sink) {
    pipeline.AddOutputSink(
        "local", std::make_unique<ObservingOutputSink>(local_sink_observer));
  }
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &observer;
  callbacks.on_start = OnProcessorStart;
  callbacks.process_video =
      arguments.passthrough_video ? ProcessVideo : nullptr;
  callbacks.on_boundary = OnProcessorBoundary;
  callbacks.on_stop = OnProcessorStop;
  pipeline.SetProcessorCallbacks(callbacks);

  std::atomic_bool running_seen = false;
  std::atomic_bool failed_seen = false;
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    events.Write("pipeline_status", {{"state", ToString(status)}});
    if (status == StreamingPipelineStatus::kRunning) {
      running_seen.store(true, std::memory_order_release);
      events.Write("output_opened",
                   {{"target_count", std::to_string(arguments.outputs.size())},
                    {"input_target_count",
                     std::to_string(arguments.input_outputs.size())}});
    } else if (status == StreamingPipelineStatus::kFailed) {
      failed_seen.store(true, std::memory_order_release);
    }
  });

  events.Write("runner_started");
  pipeline.Start();

  const auto finish_at = std::chrono::steady_clock::now() + arguments.duration;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         !failed_seen.load(std::memory_order_acquire) &&
         pipeline.status() != StreamingPipelineStatus::kStopped &&
         std::chrono::steady_clock::now() < finish_at) {
    std::this_thread::sleep_for(1s);
    events.Write("heartbeat",
                 {{"state", ToString(pipeline.status())},
                  {"timeline_reset_count",
                   std::to_string(observer.timeline_reset_count.load())}});
  }

  pipeline.Stop();
  const auto final_status = pipeline.status();
  const auto performance = pipeline.CollectPerformance();
  events.Write(
      "summary",
      {{"running_seen", running_seen.load() ? "1" : "0"},
       {"failed_seen", failed_seen.load() ? "1" : "0"},
       {"final_status", ToString(final_status)},
       {"timeline_reset_count",
        std::to_string(observer.timeline_reset_count.load())},
       {"has_audio", performance.has_audio ? "1" : "0"},
       {"has_video", performance.has_video ? "1" : "0"},
       {"audio_encode_samples",
        std::to_string(performance.audio.encode.samples)},
       {"video_encode_frames", std::to_string(performance.video.encode.frames)},
       {"local_sink_starts", std::to_string(local_sink_observer.starts.load())},
       {"local_sink_stops", std::to_string(local_sink_observer.stops.load())},
       {"local_sink_video_frames",
        std::to_string(local_sink_observer.video_frames.load())},
       {"local_sink_audio_frames",
        std::to_string(local_sink_observer.audio_frames.load())},
       {"local_sink_invalid_frames",
        std::to_string(local_sink_observer.invalid_frames.load())}});

  return running_seen.load() && !failed_seen.load() ? 0 : 2;
}

int RunRemux(const Arguments& arguments, EventWriter& events) {
  RemuxPipelineConfig config;
  config.input_url = arguments.input;
  config.output_targets = arguments.outputs;
  RemuxPipeline pipeline(std::move(config));

  std::atomic_bool running_seen = false;
  std::atomic_bool failed_seen = false;
  pipeline.SetOnStatus([&](RemuxPipelineStatus status) {
    events.Write("pipeline_status", {{"state", ToString(status)}});
    if (status == RemuxPipelineStatus::kRunning) {
      running_seen.store(true, std::memory_order_release);
      events.Write(
          "output_opened",
          {{"target_count", std::to_string(arguments.outputs.size())}});
    } else if (status == RemuxPipelineStatus::kFailed) {
      failed_seen.store(true, std::memory_order_release);
    }
  });

  events.Write("runner_started");
  pipeline.Start();

  const auto finish_at = std::chrono::steady_clock::now() + arguments.duration;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         !failed_seen.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < finish_at) {
    std::this_thread::sleep_for(1s);
    events.Write("heartbeat", {{"state", ToString(pipeline.status())}});
  }

  pipeline.Stop();
  const auto final_status = pipeline.status();
  events.Write("summary", {{"running_seen", running_seen.load() ? "1" : "0"},
                           {"failed_seen", failed_seen.load() ? "1" : "0"},
                           {"final_status", ToString(final_status)},
                           {"timeline_reset_count", "0"}});
  return running_seen.load() && !failed_seen.load() ? 0 : 2;
}

int RunFile(const Arguments& arguments, EventWriter& events) {
  FileProcessorObserver observer;
  observer.events = &events;

  LocalFilePipelineConfig config;
  config.input_path = arguments.input;
  config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  FilePipeline pipeline(std::move(config));
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &observer;
  callbacks.on_start = OnFileProcessorStart;
  callbacks.process_video = ProcessFileVideo;
  callbacks.process_audio = ProcessFileAudio;
  callbacks.on_boundary = OnFileProcessorBoundary;
  callbacks.on_stop = OnFileProcessorStop;
  pipeline.SetProcessorCallbacks(callbacks);

  std::atomic_bool running_seen = false;
  std::atomic_bool failed_seen = false;
  pipeline.SetOnStatus([&](FilePipelineStatus status) {
    events.Write("pipeline_status", {{"state", ToString(status)}});
    if (status == FilePipelineStatus::kRunning) {
      running_seen.store(true, std::memory_order_release);
    } else if (status == FilePipelineStatus::kFailed) {
      failed_seen.store(true, std::memory_order_release);
    }
  });

  events.Write("runner_started");
  pipeline.Start();

  const auto finish_at = std::chrono::steady_clock::now() + arguments.duration;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         !failed_seen.load(std::memory_order_acquire) &&
         pipeline.status() != FilePipelineStatus::kStopped &&
         std::chrono::steady_clock::now() < finish_at) {
    std::this_thread::sleep_for(10ms);
  }
  const bool timed_out = pipeline.status() != FilePipelineStatus::kStopped &&
                         !failed_seen.load(std::memory_order_acquire) &&
                         !g_stop_requested.load(std::memory_order_relaxed);

  pipeline.Stop();
  const auto final_status = pipeline.status();
  const auto performance = pipeline.CollectPerformance();
  events.Write(
      "summary",
      {{"running_seen", running_seen.load() ? "1" : "0"},
       {"failed_seen", failed_seen.load() ? "1" : "0"},
       {"timed_out", timed_out ? "1" : "0"},
       {"final_status", ToString(final_status)},
       {"timeline_reset_count", "0"},
       {"has_audio", performance.has_audio ? "1" : "0"},
       {"has_video", performance.has_video ? "1" : "0"},
       {"audio_frames", std::to_string(observer.audio_frames.load())},
       {"audio_samples", std::to_string(observer.audio_samples.load())},
       {"video_frames", std::to_string(observer.video_frames.load())},
       {"end_of_input_count",
        std::to_string(observer.end_of_input_count.load())},
       {"processor_stop_count", std::to_string(observer.stop_count.load())},
       {"audio_decode_samples",
        std::to_string(performance.audio.decode.samples)},
       {"audio_process_samples",
        std::to_string(performance.audio.process.samples)},
       {"video_decode_frames", std::to_string(performance.video.decode.frames)},
       {"video_process_frames",
        std::to_string(performance.video.process.frames)},
       {"progress_available", performance.progress_available ? "1" : "0"},
       {"processed_position_us",
        std::to_string(performance.processed_position.count())},
       {"duration_us", std::to_string(performance.duration.count())},
       {"progress", fmt::format("{:.6f}", performance.progress)},
       {"processing_speed_available",
        performance.processing_speed_available ? "1" : "0"},
       {"processing_speed",
        fmt::format("{:.6f}", performance.processing_speed)}});

  return running_seen.load() && !failed_seen.load() && !timed_out &&
                 final_status == FilePipelineStatus::kStopped &&
                 observer.end_of_input_count.load() == 1 &&
                 observer.stop_count.load() == 1
             ? 0
             : 2;
}

int Run(const Arguments& arguments) {
  mw::streamer::InitConfig init_config;
  init_config.log.console.color = false;
  init_config.log.modules.zlm = mw::streamer::log::LogLevel::kInfo;
  init_config.log.modules.streamer = mw::streamer::log::LogLevel::kInfo;
  init_config.log.modules.processor = mw::streamer::log::LogLevel::kInfo;
  mw::streamer::Init(init_config);

  EventWriter events(arguments.events_path);
  int result = 0;
  switch (arguments.pipeline) {
    case PipelineKind::kStreaming:
      result = RunStreaming(arguments, events);
      break;
    case PipelineKind::kRemux:
      result = RunRemux(arguments, events);
      break;
    case PipelineKind::kFile:
      result = RunFile(arguments, events);
      break;
  }
  mw::streamer::Shutdown();
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  try {
    return Run(ParseArguments(argc, argv));
  } catch (const std::exception& error) {
    fmt::print(stderr, "mw_streamer_e2e_runner: {}\n", error.what());
    if (mw::streamer::IsInitialized()) {
      mw::streamer::Shutdown();
    }
    return 1;
  }
}

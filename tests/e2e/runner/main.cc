#include <cuda.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/decoder/decoder_sink.h"
#include "mw/encoder/encoder_sink.h"
#include "mw/input/file_input.h"
#include "mw/input/zlm_input.h"
#include "mw/output/remux_sink.h"
#include "mw/pipeline/pipeline.h"
#include "mw/processor/analysis_processor_sink.h"
#include "mw/processor/processor.h"
#include "mw/processor/transform_processor_sink.h"
#include "mw/synchronizer/synchronizer_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoderBackend;
namespace pipeline = mw::streamer::pipeline;
namespace performance = mw::streamer::performance;
using performance::PerformanceType;

std::atomic_bool g_stop_requested = false;

void HandleSignal(int) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

const char* ToString(pipeline::PipelineState state) {
  switch (state) {
    case pipeline::PipelineState::kIdle:
      return "idle";
    case pipeline::PipelineState::kRunning:
      return "running";
    case pipeline::PipelineState::kStopping:
      return "stopping";
    case pipeline::PipelineState::kStopped:
      return "stopped";
    case pipeline::PipelineState::kFailed:
      return "failed";
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

enum class Scenario {
  kStreaming,
  kRemux,
  kFile,
};

struct Arguments {
  Scenario scenario = Scenario::kStreaming;
  std::string input;
  std::vector<std::string> outputs;
  std::vector<std::string> input_outputs;
  std::string events_path;
  std::chrono::milliseconds cache_duration{1000};
  std::chrono::milliseconds duration{10000};
  std::uint32_t frame_rate_num = 0;
  std::uint32_t frame_rate_den = 1;
  MwStreamerCodec video_codec = kMwStreamerCodecUnknown;
  std::chrono::milliseconds video_jitter_min{0};
  std::chrono::milliseconds video_jitter_max{0};
  bool passthrough_video = false;
  bool software_video = false;
  bool local_sink = false;
  bool observe_cache = false;
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

Scenario ParseScenario(const std::string& value) {
  if (value == "streaming") {
    return Scenario::kStreaming;
  }
  if (value == "remux") {
    return Scenario::kRemux;
  }
  if (value == "file") {
    return Scenario::kFile;
  }
  throw std::invalid_argument("--scenario必须是streaming、remux或file");
}

Arguments ParseArguments(int argc, char* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--scenario") {
      arguments.scenario = ParseScenario(RequireValue(argc, argv, index));
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
    } else if (option == "--observe-cache") {
      arguments.observe_cache = true;
    } else if (option == "--local-sink") {
      arguments.local_sink = true;
    } else {
      throw std::invalid_argument("未知参数: " + option);
    }
  }

  if (arguments.input.empty()) {
    throw std::invalid_argument("--input不能为空");
  }
  if (arguments.scenario == Scenario::kRemux && arguments.outputs.empty()) {
    throw std::invalid_argument("Remux场景的--output至少需要一个");
  }
  if (arguments.events_path.empty()) {
    throw std::invalid_argument("--events不能为空");
  }
  if (arguments.scenario == Scenario::kRemux &&
      !arguments.input_outputs.empty()) {
    throw std::invalid_argument("Remux场景不支持--input-output");
  }
  if (arguments.scenario == Scenario::kFile &&
      (!arguments.outputs.empty() || !arguments.input_outputs.empty())) {
    throw std::invalid_argument("File场景不支持输出目标");
  }
  if (arguments.scenario != Scenario::kStreaming && arguments.local_sink) {
    throw std::invalid_argument("只有Streaming场景支持--local-sink");
  }
  if (arguments.scenario != Scenario::kStreaming && arguments.observe_cache) {
    throw std::invalid_argument("只有Streaming场景支持--observe-cache");
  }
  if (arguments.frame_rate_den == 0 ||
      ((arguments.frame_rate_num == 0) !=
       (arguments.video_codec == kMwStreamerCodecUnknown))) {
    throw std::invalid_argument("视频帧率参数无效");
  }
  if (arguments.video_jitter_min > arguments.video_jitter_max) {
    throw std::invalid_argument("视频抖动最小值不能大于最大值");
  }
  if (arguments.video_jitter_max > 0ms && !arguments.passthrough_video) {
    throw std::invalid_argument("视频抖动测试必须启用视频透传");
  }
  if (arguments.passthrough_video &&
      arguments.video_codec == kMwStreamerCodecUnknown) {
    throw std::invalid_argument("纯音频输入不能启用视频透传");
  }
  return arguments;
}

struct ProcessorObserver {
  std::atomic_bool has_audio{false};
  std::atomic_bool has_video{false};
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

class ObservingFrameSink final : public mw::streamer::sink::Sink {
 public:
  explicit ObservingFrameSink(LocalSinkObserver& observer)
      : Sink("local", mw::streamer::sink::SinkMediaType::kFrame),
        observer_(observer) {}

  ~ObservingFrameSink() override { Stop(); }

  void OnStreamsReady(const mw::streamer::media::FrameStreamsReady&) override {
    StartMessages();
    if (started_.exchange(true)) return;
    observer_.starts.fetch_add(1, std::memory_order_relaxed);
    observer_.events->Write("local_sink_started");
  }

  void OnAudioFrame(const mw::streamer::media::FrameReady& ready) override {
    const auto& frame = ready.frame;
    if (!frame.get() || !frame->data[0] || frame->sample_rate <= 0 ||
        frame->ch_layout.nb_channels <= 0 || frame->nb_samples <= 0 ||
        frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      observer_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    observer_.audio_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void OnVideoFrame(const mw::streamer::media::FrameReady& ready) override {
    const auto& frame = ready.frame;
    if (!frame.get() || !frame->data[0] || frame->width <= 0 ||
        frame->height <= 0 || frame->format == AV_PIX_FMT_NONE ||
        frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
        frame->time_base.den <= 0) {
      observer_.invalid_frames.fetch_add(1, std::memory_order_relaxed);
    }
    observer_.video_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void OnTimelineReset(const mw::streamer::media::TimelineReset&) override {}
  void OnInputEnded(const mw::streamer::media::StreamEnded&) override {}

  void Stop() noexcept override {
    StopMessages();
    if (!started_.exchange(false)) return;
    observer_.stops.fetch_add(1, std::memory_order_relaxed);
    observer_.events->Write("local_sink_stopped");
  }

 private:
  LocalSinkObserver& observer_;
  std::atomic_bool started_{false};
};

// The input tap runs before DecoderSink submission. Both taps borrow this
// stack state; the Pipeline is destroyed before the state leaves scope.
struct CacheObservation {
  struct Track {
    int stream_index = -1;
    AVRational time_base{0, 1};
    std::optional<std::int64_t> latest_dts_us;
    bool first_frame_seen = false;
  };
  std::mutex mutex;
  std::uint64_t generation = 0;
  std::array<Track, 2> tracks;  // Audio, video.
};

class InputObservationSink final : public mw::streamer::sink::Sink {
 public:
  explicit InputObservationSink(CacheObservation& observation)
      : Sink("input_observation", mw::streamer::sink::SinkMediaType::kPacket),
        observation_(observation) {}

  void OnStreamsReady(
      const mw::streamer::media::StreamsReady& streams) override {
    std::lock_guard<std::mutex> lock(observation_.mutex);
    observation_.generation = streams.generation;
    observation_.tracks = {};
    for (const auto& stream : streams.streams) {
      const auto type = stream.codec_parameters.get()->codec_type;
      if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) continue;
      auto& track = observation_.tracks[type == AVMEDIA_TYPE_VIDEO ? 1 : 0];
      track.stream_index = stream.stream_index;
      track.time_base = stream.time_base;
    }
  }

  void OnPacket(const mw::streamer::media::PacketReady& ready) override {
    std::lock_guard<std::mutex> lock(observation_.mutex);
    if (ready.generation != observation_.generation || !ready.packet.get() ||
        ready.packet->dts == AV_NOPTS_VALUE)
      return;
    for (auto& track : observation_.tracks) {
      if (track.stream_index == ready.packet->stream_index) {
        track.latest_dts_us = av_rescale_q(ready.packet->dts, track.time_base,
                                           AVRational{1, 1000000});
        return;
      }
    }
  }

  void OnTimelineReset(
      const mw::streamer::media::TimelineReset& reset) override {
    std::lock_guard<std::mutex> lock(observation_.mutex);
    observation_.generation = reset.generation;
    observation_.tracks = {};
  }
  void OnInputEnded(const mw::streamer::media::StreamEnded&) override {}

 private:
  CacheObservation& observation_;
};

class DecodedObservationSink final : public mw::streamer::sink::Sink {
 public:
  DecodedObservationSink(CacheObservation& observation, EventWriter& events)
      : Sink("decoded_observation", mw::streamer::sink::SinkMediaType::kFrame,
             mw::streamer::sink::SinkMediaType::kFrame),
        observation_(observation),
        events_(events) {}
  ~DecodedObservationSink() override { Stop(); }

  void OnStreamsReady(
      const mw::streamer::media::FrameStreamsReady& streams) override {
    StartMessages();
    SendStreamsReady(streams);
  }
  void OnAudioFrame(const mw::streamer::media::FrameReady& frame) override {
    Observe(frame, false);
    SendAudioFrame(frame);
  }
  void OnVideoFrame(const mw::streamer::media::FrameReady& frame) override {
    Observe(frame, true);
    SendVideoFrame(frame);
  }
  void OnTimelineReset(
      const mw::streamer::media::TimelineReset& reset) override {
    SendTimelineReset(reset);
  }
  void OnInputEnded(const mw::streamer::media::StreamEnded& end) override {
    SendInputEnded(end);
  }
  void Stop() noexcept override {
    StopMessages();
    StopDownstream();
  }

 private:
  void Observe(const mw::streamer::media::FrameReady& ready, bool video) {
    std::int64_t latest_dts_us;
    const auto source_pts_us = av_rescale_q(
        ready.frame->pts, ready.frame->time_base, AVRational{1, 1000000});
    {
      std::lock_guard<std::mutex> lock(observation_.mutex);
      auto& track = observation_.tracks[video ? 1 : 0];
      if (ready.generation != observation_.generation ||
          track.first_frame_seen || !track.latest_dts_us)
        return;
      track.first_frame_seen = true;
      latest_dts_us = *track.latest_dts_us;
    }
    events_.Write(
        "processor_first_frame",
        {{"track", video ? "video" : "audio"},
         {"generation", std::to_string(ready.generation)},
         {"media_age_us", std::to_string(latest_dts_us - source_pts_us)},
         {"source_pts_us", std::to_string(source_pts_us)},
         {"input_latest_dts_us", std::to_string(latest_dts_us)}});
  }

  CacheObservation& observation_;
  EventWriter& events_;
};

struct FileProcessorObserver {
  std::atomic_bool has_audio{false};
  std::atomic_bool has_video{false};
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
  observer->has_audio.store(request->source_info->has_audio);
  observer->has_video.store(request->source_info->has_video);
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
  if (request->source_info->has_video && !request->video_output_size) {
    return kMwStreamerProcessorStartFailed;
  }
  if (request->video_output_size) {
    request->video_output_size->width = request->source_info->video.width;
    request->video_output_size->height = request->source_info->video.height;
  }
  observer->has_audio.store(request->source_info->has_audio);
  observer->has_video.store(request->source_info->has_video);
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
       {"output_width", std::to_string(request->video_output_size
                                           ? request->video_output_size->width
                                           : 0)},
       {"output_height", std::to_string(request->video_output_size
                                            ? request->video_output_size->height
                                            : 0)},
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
  CUcontext source_context = nullptr;
  ThrowIfCudaError(
      cuPointerGetAttribute(
          &source_context, CU_POINTER_ATTRIBUTE_CONTEXT,
          static_cast<CUdeviceptr>(input.storage.linear.planes[0].address)),
      "查询E2E源CUDA上下文");
  ThrowIfCudaError(cuCtxPushCurrent(source_context), "进入E2E源CUDA上下文");
  // This callback copies on a separate non-blocking stream instead of using
  // an adapter, so it must establish source readiness itself.
  const auto synchronize_result = cuCtxSynchronize();
  CUcontext popped = nullptr;
  const auto pop_result = cuCtxPopCurrent(&popped);
  ThrowIfCudaError(synchronize_result, "等待E2E源CUDA帧写入完成");
  ThrowIfCudaError(pop_result, "恢复E2E复制CUDA上下文");
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

constexpr std::pair<PerformanceType, const char*> kPerformanceTypes[] = {
    {PerformanceType::kInput, "input"},
    {PerformanceType::kAudioDecoder, "audio_decoder"},
    {PerformanceType::kVideoDecoder, "video_decoder"},
    {PerformanceType::kAudioProcessor, "audio_processor"},
    {PerformanceType::kVideoProcessor, "video_processor"},
    {PerformanceType::kSynchronizer, "synchronizer"},
    {PerformanceType::kAudioEncoder, "audio_encoder"},
    {PerformanceType::kVideoEncoder, "video_encoder"},
    {PerformanceType::kRemux, "remux"},
};

std::uint64_t InputCount(const performance::PipelineSnapshot& snapshot,
                         PerformanceType type) {
  std::uint64_t count = 0;
  for (const auto& match : snapshot.Find(type)) {
    count += match.operation->input_count;
  }
  return count;
}

void WritePerformance(const performance::PipelineSnapshot& snapshot,
                      EventWriter& events, const char* phase) {
  for (const auto& [type, name] : kPerformanceTypes) {
    for (const auto& match : snapshot.Find(type)) {
      const auto& operation = *match.operation;
      events.Write(
          "performance",
          {
              {"phase", phase},
              {"node_id", match.node->id},
              {"type", name},
              {"input_count", std::to_string(operation.input_count)},
              {"output_count", std::to_string(operation.output_count)},
              {"completed_calls", std::to_string(operation.completed_calls)},
              {"failed_calls", std::to_string(operation.failed_calls)},
              {"in_flight", std::to_string(operation.in_flight)},
              {"total_time_ns", std::to_string(operation.total_time.count())},
              {"rates_available", operation.rates_available ? "1" : "0"},
              {"input_per_second",
               fmt::format("{:.6f}", operation.input_per_second)},
              {"output_per_second",
               fmt::format("{:.6f}", operation.output_per_second)},
          });
    }
  }
}

// Non-owning probes remain valid until the owning Pipeline is destroyed.
struct SinkProbe {
  std::string id;
  std::function<bool()> ready;
  std::function<bool()> ended;
  std::function<std::string()> failure;
};

template <typename SinkType, typename State>
SinkProbe MakeProbe(SinkType& sink, State running, State ended, State failed) {
  auto* node = &sink;
  return {sink.id(),
          [node, running, ended] {
            return node->state() == running || node->state() == ended;
          },
          [node, ended] { return node->state() == ended; },
          [node, failed] {
            return node->state() == failed ? node->error() : std::string{};
          }};
}

struct RunResult {
  bool running_seen = false;
  bool failed_seen = false;
  bool eof_drained = false;
};

bool CheckFailures(pipeline::Pipeline& chain,
                   const std::vector<SinkProbe>& probes, EventWriter& events) {
  if (chain.state() == pipeline::PipelineState::kFailed) {
    events.Write("pipeline_error",
                 {{"node_id", "pipeline"}, {"error", chain.error()}});
    return true;
  }
  const auto input = chain.input_status();
  if (input.state == mw::streamer::input::InputState::kFailed &&
      !input.will_retry) {
    events.Write("pipeline_error",
                 {{"node_id", "input"}, {"error", input.error}});
    return true;
  }
  for (const auto& probe : probes) {
    const auto error = probe.failure();
    if (!error.empty()) {
      events.Write("pipeline_error", {{"node_id", probe.id}, {"error", error}});
      return true;
    }
  }
  return false;
}

RunResult RunPipeline(pipeline::Pipeline& chain, const Arguments& arguments,
                      const std::vector<SinkProbe>& probes, EventWriter& events,
                      const ProcessorObserver* observer = nullptr) {
  RunResult result;
  bool first_frame_seen = false;
  events.Write("runner_started", {{"pipeline_api", "unified"}});
  events.Write("pipeline_status", {{"state", "starting"}});
  chain.Start();
  auto previous = chain.GetPerformance();
  const auto finish_at = std::chrono::steady_clock::now() + arguments.duration;
  auto next_heartbeat = std::chrono::steady_clock::now();
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < finish_at) {
    if (CheckFailures(chain, probes, events)) {
      result.failed_seen = true;
      events.Write("pipeline_status", {{"state", "failed"}});
      break;
    }
    if (observer && !arguments.observe_cache && !first_frame_seen) {
      const auto snapshot = chain.GetPerformance();
      if (InputCount(snapshot, PerformanceType::kAudioProcessor) != 0 ||
          InputCount(snapshot, PerformanceType::kVideoProcessor) != 0) {
        first_frame_seen = true;
        events.Write("processor_first_frame");
      }
    }
    const bool ready =
        !result.running_seen &&
        std::all_of(probes.begin(), probes.end(),
                    [](const auto& probe) { return probe.ready(); });
    if (!result.running_seen && ready) {
      result.running_seen = true;
      events.Write("pipeline_status", {{"state", "running"}});
      if (!arguments.outputs.empty() || !arguments.input_outputs.empty()) {
        events.Write(
            "output_opened",
            {{"target_count", std::to_string(arguments.outputs.size())},
             {"input_target_count",
              std::to_string(arguments.input_outputs.size())}});
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_heartbeat) {
      auto current = chain.GetPerformance();
      WritePerformance(current.WithRatesSince(previous), events, "interval");
      previous = std::move(current);
      events.Write(
          "heartbeat",
          {{"state", result.running_seen ? "running" : "starting"},
           {"timeline_reset_count",
            std::to_string(observer ? observer->timeline_reset_count.load()
                                    : 0)}});
      next_heartbeat = now + 1s;
    }
    if (chain.input_status().state == mw::streamer::input::InputState::kEnded &&
        std::all_of(probes.begin(), probes.end(),
                    [](const auto& probe) { return probe.ended(); })) {
      result.eof_drained = true;
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  chain.Stop();
  result.failed_seen =
      CheckFailures(chain, probes, events) || result.failed_seen;
  events.Write("pipeline_status",
               {{"state", result.failed_seen ? "failed" : "stopped"}});
  auto final_snapshot = chain.GetPerformance();
  WritePerformance(final_snapshot, events, "final");
  return result;
}

std::unique_ptr<mw::streamer::output::RemuxSink> MakeRemux(
    const std::string& id, const std::string& target,
    std::vector<SinkProbe>& probes) {
  mw::streamer::output::RemuxSinkConfig config;
  config.target = target;
  auto sink =
      std::make_unique<mw::streamer::output::RemuxSink>(id, std::move(config));
  auto probe = MakeProbe(*sink, mw::streamer::sink::PacketSinkState::kRunning,
                         mw::streamer::sink::PacketSinkState::kEnded,
                         mw::streamer::sink::PacketSinkState::kFailed);
  auto* node = sink.get();
  const bool network = target.find("://") != std::string::npos;
  probe.ready = [node, network] {
    if (node->state() == mw::streamer::sink::PacketSinkState::kEnded)
      return true;
    if (node->state() != mw::streamer::sink::PacketSinkState::kRunning)
      return false;
    const auto snapshot = node->GetPerformance();
    return !snapshot.operations.empty() &&
           snapshot.operations.front().input_count > 0 &&
           (!network || node->GetNetworkOutputSnapshot().connected);
  };
  probes.push_back(std::move(probe));
  return sink;
}

MwStreamerProcessorStartResult OnAnalysisStart(
    const MwStreamerFileProcessorStartRequest* request, void* user_context) {
  if (!request) return kMwStreamerProcessorStartFailed;
  MwStreamerStreamingProcessorConfig config{};
  config.config = request->config->config;
  MwStreamerVideoOutputSize video_output_size{};
  const MwStreamerStreamingProcessorStartRequest adapted{
      request->source_info, &config, request->execution, &video_output_size};
  return OnProcessorStart(&adapted, user_context);
}

std::unique_ptr<mw::streamer::encoder::EncoderSink> MakeEncoder(
    const Arguments& arguments, AVRational frame_rate,
    std::vector<SinkProbe>& probes) {
  mw::streamer::encoder::EncoderSinkConfig config;
  config.video_encoder.frame_rate = {frame_rate.num, frame_rate.den};
  if (arguments.video_codec != kMwStreamerCodecUnknown) {
    config.video_encoder.codec = arguments.video_codec;
  }
  auto encoder =
      std::make_unique<mw::streamer::encoder::EncoderSink>("encoder", config);
  probes.push_back(MakeProbe(*encoder,
                             mw::streamer::encoder::EncoderSinkState::kRunning,
                             mw::streamer::encoder::EncoderSinkState::kEnded,
                             mw::streamer::encoder::EncoderSinkState::kFailed));
  for (std::size_t index = 0; index < arguments.outputs.size(); ++index) {
    encoder->AddSink(MakeRemux(fmt::format("output_{}", index),
                               arguments.outputs[index], probes));
  }
  return encoder;
}

std::unique_ptr<mw::streamer::synchronizer::SynchronizerSink> MakeSynchronizer(
    const Arguments& arguments, LocalSinkObserver& observer,
    std::vector<SinkProbe>& probes) {
  auto synchronizer =
      std::make_unique<mw::streamer::synchronizer::SynchronizerSink>(
          "synchronizer");
  auto probe =
      MakeProbe(*synchronizer,
                mw::streamer::synchronizer::SynchronizerSinkState::kRunning,
                mw::streamer::synchronizer::SynchronizerSinkState::kEnded,
                mw::streamer::synchronizer::SynchronizerSinkState::kFailed);
  auto* node = synchronizer.get();
  probe.ready = [node] {
    return node->state() ==
               mw::streamer::synchronizer::SynchronizerSinkState::kRunning ||
           node->state() ==
               mw::streamer::synchronizer::SynchronizerSinkState::kStandby ||
           node->state() ==
               mw::streamer::synchronizer::SynchronizerSinkState::kEnded;
  };
  probes.push_back(std::move(probe));
  if (!arguments.outputs.empty()) {
    synchronizer->AddSink(
        MakeEncoder(arguments,
                    {static_cast<int>(arguments.frame_rate_num),
                     static_cast<int>(arguments.frame_rate_den)},
                    probes));
  }
  if (arguments.local_sink) {
    synchronizer->AddSink(std::make_unique<ObservingFrameSink>(observer));
  }
  return synchronizer;
}

std::unique_ptr<mw::streamer::sink::Sink> MakeProcessor(
    const Arguments& arguments, ProcessorObserver& observer,
    LocalSinkObserver& local_observer, std::vector<SinkProbe>& probes) {
  if (arguments.outputs.empty() && !arguments.local_sink) {
    MwStreamerFileProcessorCallbacks callbacks{};
    callbacks.user_context = &observer;
    callbacks.on_start = OnAnalysisStart;
    callbacks.on_boundary = OnProcessorBoundary;
    callbacks.on_stop = OnProcessorStop;
    return std::make_unique<mw::streamer::processor::AnalysisProcessorSink>(
        "processor", callbacks);
  }
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &observer;
  callbacks.on_start = OnProcessorStart;
  callbacks.process_video =
      arguments.passthrough_video ? ProcessVideo : nullptr;
  callbacks.on_boundary = OnProcessorBoundary;
  callbacks.on_stop = OnProcessorStop;
  auto processor =
      std::make_unique<mw::streamer::processor::TransformProcessorSink>(
          "processor", callbacks);
  processor->AddSink(MakeSynchronizer(arguments, local_observer, probes));
  return processor;
}

int RunStreaming(const Arguments& arguments, EventWriter& events) {
  ProcessorObserver observer;
  observer.events = &events;
  observer.video_jitter_min = arguments.video_jitter_min;
  observer.video_jitter_max = arguments.video_jitter_max;
  LocalSinkObserver local_sink_observer;
  local_sink_observer.events = &events;
  CacheObservation cache_observation;
  mw::streamer::input::ZlmInputConfig input;
  input.url = arguments.input;
  pipeline::Pipeline chain(
      std::make_unique<mw::streamer::input::ZlmInput>(input));
  if (arguments.observe_cache) {
    chain.AddSink(std::make_unique<InputObservationSink>(cache_observation));
  }
  std::vector<SinkProbe> probes;
  mw::streamer::decoder::DecoderSinkConfig decoder_config;
  decoder_config.cache_duration = arguments.cache_duration;
  if (arguments.software_video) {
    decoder_config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  }
  auto decoder = std::make_unique<mw::streamer::decoder::DecoderSink>(
      "decoder", decoder_config);
  probes.push_back(MakeProbe(*decoder,
                             mw::streamer::sink::PacketSinkState::kRunning,
                             mw::streamer::sink::PacketSinkState::kEnded,
                             mw::streamer::sink::PacketSinkState::kFailed));
  auto processor =
      MakeProcessor(arguments, observer, local_sink_observer, probes);
  if (arguments.observe_cache) {
    auto decoded =
        std::make_unique<DecodedObservationSink>(cache_observation, events);
    decoded->AddSink(std::move(processor));
    decoder->AddSink(std::move(decoded));
  } else {
    decoder->AddSink(std::move(processor));
  }
  chain.AddSink(std::move(decoder));
  for (std::size_t index = 0; index < arguments.input_outputs.size(); ++index) {
    chain.AddSink(MakeRemux(fmt::format("input_output_{}", index),
                            arguments.input_outputs[index], probes));
  }
  const auto result = RunPipeline(chain, arguments, probes, events, &observer);
  const auto snapshot = chain.GetPerformance();
  events.Write("summary",
               {
                   {"running_seen", result.running_seen ? "1" : "0"},
                   {"failed_seen", result.failed_seen ? "1" : "0"},
                   {"final_status", ToString(chain.state())},
                   {"timeline_reset_count",
                    std::to_string(observer.timeline_reset_count.load())},
                   {"has_audio", observer.has_audio.load() ? "1" : "0"},
                   {"has_video", observer.has_video.load() ? "1" : "0"},
                   {"audio_encode_samples",
                    std::to_string(
                        InputCount(snapshot, PerformanceType::kAudioEncoder))},
                   {"video_encode_frames",
                    std::to_string(
                        InputCount(snapshot, PerformanceType::kVideoEncoder))},
                   {"audio_process_samples",
                    std::to_string(InputCount(
                        snapshot, PerformanceType::kAudioProcessor))},
                   {"video_process_frames",
                    std::to_string(InputCount(
                        snapshot, PerformanceType::kVideoProcessor))},
                   {"local_sink_starts",
                    std::to_string(local_sink_observer.starts.load())},
                   {"local_sink_stops",
                    std::to_string(local_sink_observer.stops.load())},
                   {"local_sink_video_frames",
                    std::to_string(local_sink_observer.video_frames.load())},
                   {"local_sink_audio_frames",
                    std::to_string(local_sink_observer.audio_frames.load())},
                   {"local_sink_invalid_frames",
                    std::to_string(local_sink_observer.invalid_frames.load())},
               });
  return result.running_seen && !result.failed_seen ? 0 : 2;
}

int RunRemux(const Arguments& arguments, EventWriter& events) {
  mw::streamer::input::ZlmInputConfig input;
  input.url = arguments.input;
  pipeline::Pipeline chain(
      std::make_unique<mw::streamer::input::ZlmInput>(input));
  std::vector<SinkProbe> probes;
  for (std::size_t index = 0; index < arguments.outputs.size(); ++index) {
    chain.AddSink(MakeRemux(fmt::format("output_{}", index),
                            arguments.outputs[index], probes));
  }
  const auto result = RunPipeline(chain, arguments, probes, events);
  events.Write("summary", {
                              {"running_seen", result.running_seen ? "1" : "0"},
                              {"failed_seen", result.failed_seen ? "1" : "0"},
                              {"final_status", ToString(chain.state())},
                              {"timeline_reset_count", "0"},
                          });
  return result.running_seen && !result.failed_seen ? 0 : 2;
}

std::uint64_t OutputCount(const performance::PipelineSnapshot& snapshot,
                          PerformanceType type) {
  std::uint64_t count = 0;
  for (const auto& match : snapshot.Find(type)) {
    count += match.operation->output_count;
  }
  return count;
}

int RunFile(const Arguments& arguments, EventWriter& events) {
  FileProcessorObserver observer;
  observer.events = &events;
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &observer;
  callbacks.on_start = OnFileProcessorStart;
  callbacks.process_video = ProcessFileVideo;
  callbacks.process_audio = ProcessFileAudio;
  callbacks.on_boundary = OnFileProcessorBoundary;
  callbacks.on_stop = OnFileProcessorStop;
  mw::streamer::input::FileInputConfig input{arguments.input};
  pipeline::Pipeline chain(
      std::make_unique<mw::streamer::input::FileInput>(input));
  mw::streamer::decoder::DecoderSinkConfig decoder_config;
  decoder_config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  auto decoder = std::make_unique<mw::streamer::decoder::DecoderSink>(
      "decoder", decoder_config);
  std::vector<SinkProbe> probes;
  probes.push_back(MakeProbe(*decoder,
                             mw::streamer::sink::PacketSinkState::kRunning,
                             mw::streamer::sink::PacketSinkState::kEnded,
                             mw::streamer::sink::PacketSinkState::kFailed));
  decoder->AddSink(
      std::make_unique<mw::streamer::processor::AnalysisProcessorSink>(
          "processor", callbacks));
  chain.AddSink(std::move(decoder));
  const auto result = RunPipeline(chain, arguments, probes, events);
  const bool timed_out = !result.eof_drained && !result.failed_seen &&
                         !g_stop_requested.load(std::memory_order_relaxed);
  const auto snapshot = chain.GetPerformance();
  events.Write(
      "summary",
      {
          {"running_seen", result.running_seen ? "1" : "0"},
          {"failed_seen", result.failed_seen ? "1" : "0"},
          {"timed_out", timed_out ? "1" : "0"},
          {"final_status", ToString(chain.state())},
          {"timeline_reset_count", "0"},
          {"has_audio", observer.has_audio.load() ? "1" : "0"},
          {"has_video", observer.has_video.load() ? "1" : "0"},
          {"audio_frames", std::to_string(observer.audio_frames.load())},
          {"audio_samples", std::to_string(observer.audio_samples.load())},
          {"video_frames", std::to_string(observer.video_frames.load())},
          {"end_of_input_count",
           std::to_string(observer.end_of_input_count.load())},
          {"processor_stop_count", std::to_string(observer.stop_count.load())},
          {"audio_decode_samples",
           std::to_string(
               OutputCount(snapshot, PerformanceType::kAudioDecoder))},
          {"audio_process_samples",
           std::to_string(
               InputCount(snapshot, PerformanceType::kAudioProcessor))},
          {"video_decode_frames",
           std::to_string(
               OutputCount(snapshot, PerformanceType::kVideoDecoder))},
          {"video_process_frames",
           std::to_string(
               InputCount(snapshot, PerformanceType::kVideoProcessor))},
      });
  return result.running_seen && !result.failed_seen && result.eof_drained &&
                 observer.end_of_input_count.load() == 1 &&
                 observer.stop_count.load() == 1
             ? 0
             : 2;
}

int Run(const Arguments& arguments) {
  EventWriter events(arguments.events_path);
  int result = 0;
  switch (arguments.scenario) {
    case Scenario::kStreaming:
      result = RunStreaming(arguments, events);
      break;
    case Scenario::kRemux:
      result = RunRemux(arguments, events);
      break;
    case Scenario::kFile:
      result = RunFile(arguments, events);
      break;
  }
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
    return 1;
  }
}

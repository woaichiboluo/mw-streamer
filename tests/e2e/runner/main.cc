#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
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
#include "mw/pipeline/streaming_pipeline.h"
#include "mw/processor/processor.h"

namespace {

using namespace std::chrono_literals;
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

struct Arguments {
  std::string input;
  std::vector<std::string> outputs;
  std::string events_path;
  std::chrono::milliseconds cache_duration{1000};
  std::chrono::milliseconds duration{10000};
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::uint32_t frame_rate_num = 0;
  std::uint32_t frame_rate_den = 1;
  MwStreamerCodec video_codec = kMwStreamerCodecUnknown;
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

Arguments ParseArguments(int argc, char* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--input") {
      arguments.input = RequireValue(argc, argv, index);
    } else if (option == "--output") {
      arguments.outputs.push_back(RequireValue(argc, argv, index));
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
    } else {
      throw std::invalid_argument("未知参数: " + option);
    }
  }

  if (arguments.input.empty()) {
    throw std::invalid_argument("--input不能为空");
  }
  if (arguments.outputs.empty()) {
    throw std::invalid_argument("--output至少需要一个");
  }
  if (arguments.events_path.empty()) {
    throw std::invalid_argument("--events不能为空");
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
  return arguments;
}

struct ProcessorObserver {
  EventWriter* events = nullptr;
  std::atomic_uint64_t timeline_reset_count{0};
};

MwStreamerProcessorStartResult OnProcessorStart(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context) {
  auto* observer = static_cast<ProcessorObserver*>(user_context);
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
       {"output_width", std::to_string(request->config->output_width)},
       {"output_height", std::to_string(request->config->output_height)},
       {"execution", request->execution->type == kMwStreamerExecutionCuda
                         ? "cuda"
                         : "cpu"}});
  return kMwStreamerProcessorStartSuccess;
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
  if (observer && observer->events) {
    observer->events->Write("processor_stopped");
  }
}

int Run(const Arguments& arguments) {
  mw::streamer::InitConfig init_config;
  init_config.log.console.color = false;
  init_config.log.modules.zlm = mw::streamer::log::LogLevel::kInfo;
  init_config.log.modules.streamer = mw::streamer::log::LogLevel::kInfo;
  init_config.log.modules.processor = mw::streamer::log::LogLevel::kInfo;
  mw::streamer::Init(init_config);

  EventWriter events(arguments.events_path);
  ProcessorObserver observer;
  observer.events = &events;

  StreamingPipelineConfig config;
  config.input_url = arguments.input;
  config.output_targets = arguments.outputs;
  config.cache_duration = arguments.cache_duration;
  config.processor.output_width = arguments.output_width;
  config.processor.output_height = arguments.output_height;
  config.video_encoder.frame_rate = {
      static_cast<std::int32_t>(arguments.frame_rate_num),
      static_cast<std::int32_t>(arguments.frame_rate_den),
  };
  config.video_encoder.codec = arguments.video_codec;

  StreamingPipeline pipeline(std::move(config));
  const MwStreamerStreamingProcessorCallbacks callbacks{
      &observer,           OnProcessorStart, nullptr,         nullptr,
      OnProcessorBoundary, nullptr,          OnProcessorStop,
  };
  pipeline.SetProcessorCallbacks(callbacks);

  std::atomic_bool running_seen = false;
  std::atomic_bool failed_seen = false;
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    events.Write("pipeline_status", {{"state", ToString(status)}});
    if (status == StreamingPipelineStatus::kRunning) {
      running_seen.store(true, std::memory_order_release);
      events.Write(
          "output_opened",
          {{"target_count", std::to_string(arguments.outputs.size())}});
    } else if (status == StreamingPipelineStatus::kFailed) {
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
    events.Write("heartbeat",
                 {{"state", ToString(pipeline.status())},
                  {"timeline_reset_count",
                   std::to_string(observer.timeline_reset_count.load())}});
  }

  pipeline.Stop();
  const auto final_status = pipeline.status();
  events.Write("summary",
               {{"running_seen", running_seen.load() ? "1" : "0"},
                {"failed_seen", failed_seen.load() ? "1" : "0"},
                {"final_status", ToString(final_status)},
                {"timeline_reset_count",
                 std::to_string(observer.timeline_reset_count.load())}});

  mw::streamer::Shutdown();
  return running_seen.load() && !failed_seen.load() ? 0 : 2;
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

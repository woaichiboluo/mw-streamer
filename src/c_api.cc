#include "mw/c_api.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mw/config/toml.h"
#include "mw/ffmpeg/frame_view.h"
#include "mw/init/init.h"
#include "mw/output/output_sink.h"
#include "mw/performance/snapshot.h"
#include "mw/pipeline/file_pipeline.h"
#include "mw/pipeline/remux_pipeline.h"
#include "mw/pipeline/streaming_pipeline.h"

namespace pipeline = mw::streamer::pipeline;
namespace performance = mw::streamer::performance;

struct MwStreaming {
  MwStreaming(std::filesystem::path config_path,
              pipeline::StreamingPipelineConfig config)
      : config_path(std::move(config_path)), pipeline(std::move(config)) {}

  MwStatusCallback on_status = nullptr;
  void* status_context = nullptr;
  std::filesystem::path config_path;
  // Keep the Pipeline last so it is destroyed before the callback state that
  // its final Stop transition may access.
  pipeline::StreamingPipeline pipeline;
};

struct MwRemux {
  explicit MwRemux(pipeline::RemuxPipelineConfig config)
      : pipeline(std::move(config)) {}

  MwStatusCallback on_status = nullptr;
  void* status_context = nullptr;
  pipeline::RemuxPipeline pipeline;
};

struct MwFile {
  MwFile(std::filesystem::path config_path,
         pipeline::LocalFilePipelineConfig config)
      : config_path(std::move(config_path)), pipeline(std::move(config)) {}

  MwStatusCallback on_status = nullptr;
  void* status_context = nullptr;
  std::filesystem::path config_path;
  pipeline::FilePipeline pipeline;
};

namespace {

thread_local std::string last_error;
thread_local const char* fallback_error = "";

void ClearError() noexcept {
  fallback_error = "";
  try {
    last_error.clear();
  } catch (...) {
  }
}

void SetError(const char* message) noexcept {
  try {
    last_error = message ? message : "未知错误";
    fallback_error = "";
  } catch (...) {
    last_error.clear();
    fallback_error = "无法保存错误信息";
  }
}

template <typename Function>
MwResult Guard(Function&& function) noexcept {
  ClearError();
  try {
    std::forward<Function>(function)();
    return kMwResultSuccess;
  } catch (const std::invalid_argument& error) {
    SetError(error.what());
    return kMwResultInvalidArgument;
  } catch (const std::logic_error& error) {
    SetError(error.what());
    return kMwResultInvalidState;
  } catch (const std::bad_alloc& error) {
    SetError(error.what());
    return kMwResultOutOfMemory;
  } catch (const std::exception& error) {
    SetError(error.what());
    return kMwResultInternalError;
  } catch (...) {
    SetError("未知异常");
    return kMwResultInternalError;
  }
}

template <typename Config, typename Loader, typename Consumer>
MwResult LoadConfig(const std::filesystem::path& path, Loader&& loader,
                    Consumer&& consumer) noexcept {
  ClearError();
  std::optional<Config> config;
  try {
    config.emplace(std::forward<Loader>(loader)(path));
  } catch (const std::bad_alloc& error) {
    SetError(error.what());
    return kMwResultOutOfMemory;
  } catch (const std::exception& error) {
    SetError(error.what());
    return kMwResultConfigError;
  } catch (...) {
    SetError("加载配置时发生未知异常");
    return kMwResultConfigError;
  }
  return Guard(
      [&]() { std::forward<Consumer>(consumer)(std::move(*config), path); });
}

template <typename Config, typename Loader, typename Consumer>
MwResult LoadConfig(const char* path, Loader&& loader,
                    Consumer&& consumer) noexcept {
  ClearError();
  if (!path) {
    SetError("配置文件路径不能为空");
    return kMwResultInvalidArgument;
  }

  try {
    return LoadConfig<Config>(
        std::filesystem::absolute(std::filesystem::path(path))
            .lexically_normal(),
        std::forward<Loader>(loader), std::forward<Consumer>(consumer));
  } catch (const std::bad_alloc& error) {
    SetError(error.what());
    return kMwResultOutOfMemory;
  } catch (const std::exception& error) {
    SetError(error.what());
    return kMwResultConfigError;
  } catch (...) {
    SetError("解析配置文件路径时发生未知异常");
    return kMwResultConfigError;
  }
}

template <typename T>
T& Require(T* pointer, const char* name) {
  if (!pointer) {
    throw std::invalid_argument(std::string(name) + "不能为空");
  }
  return *pointer;
}

MwPipelineStatus ToStatus(pipeline::StreamingPipelineStatus status) {
  switch (status) {
    case pipeline::StreamingPipelineStatus::kIdle:
      return kMwPipelineStatusIdle;
    case pipeline::StreamingPipelineStatus::kStarting:
      return kMwPipelineStatusStarting;
    case pipeline::StreamingPipelineStatus::kRunning:
      return kMwPipelineStatusRunning;
    case pipeline::StreamingPipelineStatus::kFailed:
      return kMwPipelineStatusFailed;
    case pipeline::StreamingPipelineStatus::kStopped:
      return kMwPipelineStatusStopped;
  }
  return kMwPipelineStatusFailed;
}

MwPipelineStatus ToStatus(pipeline::RemuxPipelineStatus status) {
  switch (status) {
    case pipeline::RemuxPipelineStatus::kIdle:
      return kMwPipelineStatusIdle;
    case pipeline::RemuxPipelineStatus::kStarting:
      return kMwPipelineStatusStarting;
    case pipeline::RemuxPipelineStatus::kRunning:
      return kMwPipelineStatusRunning;
    case pipeline::RemuxPipelineStatus::kFailed:
      return kMwPipelineStatusFailed;
    case pipeline::RemuxPipelineStatus::kStopped:
      return kMwPipelineStatusStopped;
  }
  return kMwPipelineStatusFailed;
}

MwPipelineStatus ToStatus(pipeline::FilePipelineStatus status) {
  switch (status) {
    case pipeline::FilePipelineStatus::kIdle:
      return kMwPipelineStatusIdle;
    case pipeline::FilePipelineStatus::kStarting:
      return kMwPipelineStatusStarting;
    case pipeline::FilePipelineStatus::kRunning:
      return kMwPipelineStatusRunning;
    case pipeline::FilePipelineStatus::kFailed:
      return kMwPipelineStatusFailed;
    case pipeline::FilePipelineStatus::kStopped:
      return kMwPipelineStatusStopped;
  }
  return kMwPipelineStatusFailed;
}

MwLatencyStats ToStats(const performance::LatencySnapshot& source) {
  return {
      source.sample_count, source.p50.count(), source.p95.count(),
      source.p99.count(),  source.max.count(),
  };
}

MwVideoStageStats ToStats(const performance::VideoStageSnapshot& source) {
  return {source.frames, source.frames_per_second, ToStats(source.latency)};
}

MwAudioStageStats ToStats(const performance::AudioStageSnapshot& source) {
  return {source.samples, source.samples_per_second, ToStats(source.latency)};
}

MwNetworkInputStats ToStats(const performance::NetworkInputSnapshot& source) {
  return {
      static_cast<uint8_t>(source.is_network),
      static_cast<uint8_t>(source.connected),
      source.generation,
      source.reconnect_count,
      source.received_bytes,
  };
}

void DestroyOutputs(MwNetworkOutputStats* outputs, size_t count) noexcept {
  if (!outputs) {
    return;
  }
  for (size_t index = 0; index < count; ++index) {
    delete[] outputs[index].target;
  }
  delete[] outputs;
}

MwNetworkOutputStats* CopyOutputs(
    const std::vector<performance::NetworkOutputSnapshot>& source) {
  if (source.empty()) {
    return nullptr;
  }

  auto outputs = std::make_unique<MwNetworkOutputStats[]>(source.size());
  size_t completed = 0;
  try {
    for (; completed < source.size(); ++completed) {
      const auto& input = source[completed];
      auto target = std::make_unique<char[]>(input.target.size() + 1);
      std::copy(input.target.begin(), input.target.end(), target.get());
      target[input.target.size()] = '\0';
      outputs[completed] = {
          target.release(),
          static_cast<uint8_t>(input.connected),
          input.reconnect_count,
          input.sent_bytes,
      };
    }
  } catch (...) {
    DestroyOutputs(outputs.release(), completed);
    throw;
  }
  return outputs.release();
}

MwStreamingStats* MakeStats(
    const performance::StreamingPipelineSnapshot& source) {
  auto stats = std::make_unique<MwStreamingStats>();
  stats->interval_ns = source.interval.count();
  stats->input = ToStats(source.input);
  stats->output_count = source.outputs.size();
  stats->outputs = CopyOutputs(source.outputs);
  stats->has_video = static_cast<uint8_t>(source.has_video);
  stats->video = {
      ToStats(source.video.decode), ToStats(source.video.process),
      ToStats(source.video.encode), source.video.dropped_packets,
      source.video.queue_depth,
  };
  stats->has_audio = static_cast<uint8_t>(source.has_audio);
  stats->audio = {
      ToStats(source.audio.decode), ToStats(source.audio.process),
      ToStats(source.audio.encode), source.audio.dropped_packets,
      source.audio.queue_depth,
  };
  stats->output_queue_depth = source.output_queue_depth;
  return stats.release();
}

MwRemuxStats* MakeStats(const performance::RemuxPipelineSnapshot& source) {
  auto stats = std::make_unique<MwRemuxStats>();
  stats->interval_ns = source.interval.count();
  stats->input = ToStats(source.input);
  stats->output_count = source.outputs.size();
  stats->outputs = CopyOutputs(source.outputs);
  stats->packets = source.packets;
  stats->bytes = source.bytes;
  stats->bits_per_second = source.bits_per_second;
  stats->output_queue_depth = source.output_queue_depth;
  return stats.release();
}

MwFileStats* MakeStats(const performance::LocalFilePipelineSnapshot& source) {
  auto stats = std::make_unique<MwFileStats>();
  stats->interval_ns = source.interval.count();
  stats->progress_available = static_cast<uint8_t>(source.progress_available);
  stats->processed_position_us = source.processed_position.count();
  stats->duration_us = source.duration.count();
  stats->progress = source.progress;
  stats->processing_speed_available =
      static_cast<uint8_t>(source.processing_speed_available);
  stats->processing_speed = source.processing_speed;
  stats->has_video = static_cast<uint8_t>(source.has_video);
  stats->video = {
      ToStats(source.video.decode),
      ToStats(source.video.process),
  };
  stats->has_audio = static_cast<uint8_t>(source.has_audio);
  stats->audio = {
      ToStats(source.audio.decode),
      ToStats(source.audio.process),
  };
  return stats.release();
}

class CallbackOutputSink final : public mw::streamer::output::OutputSink {
 public:
  explicit CallbackOutputSink(MwStreamerOutputSinkCallbacks callbacks)
      : callbacks_(callbacks) {}

  void Start() override {
    if (callbacks_.start) {
      callbacks_.start(callbacks_.user_context);
    }
  }

  void WriteAudio(mw::streamer::ffmpeg::Frame frame) override {
    if (callbacks_.write_audio) {
      const mw::streamer::ffmpeg::AudioFrameViewAdapter adapter(frame);
      callbacks_.write_audio(&adapter.view(), callbacks_.user_context);
    }
  }

  void WriteVideo(mw::streamer::ffmpeg::Frame frame) override {
    if (callbacks_.write_video) {
      const mw::streamer::ffmpeg::VideoFrameViewAdapter adapter(frame);
      callbacks_.write_video(&adapter.view(), callbacks_.user_context);
    }
  }

  void Stop() noexcept override {
    if (callbacks_.stop) {
      callbacks_.stop(callbacks_.user_context);
    }
  }

 private:
  const MwStreamerOutputSinkCallbacks callbacks_;
};

}  // namespace

extern "C" {

const char* mw_last_error(void) {
  return last_error.empty() ? fallback_error : last_error.c_str();
}

MwResult mw_init(const char* config_path) {
  if (!config_path) {
    return Guard([]() { mw::streamer::Init(); });
  }
  return LoadConfig<mw::streamer::InitConfig>(
      config_path, mw::streamer::config::LoadInitConfigFromToml,
      [](mw::streamer::InitConfig config, const std::filesystem::path&) {
        mw::streamer::Init(config);
      });
}

void mw_shutdown(void) {
  ClearError();
  mw::streamer::Shutdown();
}

MwResult mw_streaming_create(const char* config_path, MwStreaming** output) {
  if (output) {
    *output = nullptr;
  }
  if (!output) {
    return Guard([]() { throw std::invalid_argument("输出句柄不能为空"); });
  }
  return LoadConfig<pipeline::StreamingPipelineConfig>(
      config_path, mw::streamer::config::LoadStreamingPipelineConfigFromToml,
      [output](pipeline::StreamingPipelineConfig config,
               const std::filesystem::path& absolute_path) {
        *output = new MwStreaming(absolute_path, std::move(config));
      });
}

MwResult mw_streaming_on_status(MwStreaming* streaming,
                                MwStatusCallback callback, void* user_context) {
  return Guard([&]() {
    auto& handle = Require(streaming, "Streaming句柄");
    handle.pipeline.SetOnStatus([streaming](auto status) {
      if (streaming->on_status) {
        streaming->on_status(ToStatus(status), streaming->status_context);
      }
    });
    handle.on_status = callback;
    handle.status_context = user_context;
  });
}

MwResult mw_streaming_set_processor(
    MwStreaming* streaming,
    const MwStreamerStreamingProcessorCallbacks* callbacks) {
  return Guard([&]() {
    Require(streaming, "Streaming句柄")
        .pipeline.SetProcessorCallbacks(Require(callbacks, "Processor回调"));
  });
}

MwResult mw_streaming_add_output_sink(
    MwStreaming* streaming, const char* sink_id,
    const MwStreamerOutputSinkCallbacks* callbacks) {
  return Guard([&]() {
    if (!sink_id) {
      throw std::invalid_argument("Output Sink ID不能为空");
    }
    auto sink = std::make_unique<CallbackOutputSink>(
        Require(callbacks, "Output Sink回调"));
    Require(streaming, "Streaming句柄")
        .pipeline.AddOutputSink(sink_id, std::move(sink));
  });
}

MwResult mw_streaming_start(MwStreaming* streaming) {
  return Guard([&]() { Require(streaming, "Streaming句柄").pipeline.Start(); });
}

MwResult mw_streaming_reload(MwStreaming* streaming) {
  if (!streaming) {
    return Guard(
        []() { throw std::invalid_argument("Streaming句柄不能为空"); });
  }
  return LoadConfig<pipeline::StreamingPipelineConfig>(
      streaming->config_path,
      mw::streamer::config::LoadStreamingPipelineConfigFromToml,
      [streaming](pipeline::StreamingPipelineConfig config,
                  const std::filesystem::path&) {
        streaming->pipeline.UpdateProcessorConfig(
            std::move(config.processor.config));
      });
}

MwResult mw_streaming_submit_output_event(MwStreaming* streaming,
                                          const MwStreamerOutputEvent* event) {
  pipeline::OutputEventSubmitResult result =
      pipeline::OutputEventSubmitResult::kUnavailable;
  const auto call_result = Guard([&]() {
    result = Require(streaming, "Streaming句柄")
                 .pipeline.SubmitOutputEvent(Require(event, "Output Event"));
  });
  if (call_result != kMwResultSuccess) {
    return call_result;
  }
  switch (result) {
    case pipeline::OutputEventSubmitResult::kAccepted:
      return kMwResultSuccess;
    case pipeline::OutputEventSubmitResult::kQueueFull:
      SetError("Processor输出事件队列已满");
      return kMwResultQueueFull;
    case pipeline::OutputEventSubmitResult::kUnavailable:
      SetError("Processor输出事件当前不可用");
      return kMwResultInvalidState;
  }
  SetError("未知Output Event提交结果");
  return kMwResultInternalError;
}

MwResult mw_streaming_status(const MwStreaming* streaming,
                             MwPipelineStatus* output) {
  return Guard([&]() {
    Require(output, "状态输出") =
        ToStatus(Require(streaming, "Streaming句柄").pipeline.status());
  });
}

MwResult mw_streaming_stats(MwStreaming* streaming, MwStreamingStats** output) {
  if (output) {
    *output = nullptr;
  }
  return Guard([&]() {
    auto& destination = Require(output, "性能统计输出");
    destination = MakeStats(
        Require(streaming, "Streaming句柄").pipeline.CollectPerformance());
  });
}

void mw_streaming_stats_destroy(MwStreamingStats* stats) {
  ClearError();
  if (stats) {
    DestroyOutputs(stats->outputs, stats->output_count);
    delete stats;
  }
}

void mw_streaming_stop(MwStreaming* streaming) {
  ClearError();
  if (streaming) {
    streaming->pipeline.Stop();
  } else {
    SetError("Streaming句柄不能为空");
  }
}

void mw_streaming_destroy(MwStreaming* streaming) {
  ClearError();
  delete streaming;
}

MwResult mw_remux_create(const char* config_path, MwRemux** output) {
  if (output) {
    *output = nullptr;
  }
  if (!output) {
    return Guard([]() { throw std::invalid_argument("输出句柄不能为空"); });
  }
  return LoadConfig<pipeline::RemuxPipelineConfig>(
      config_path, mw::streamer::config::LoadRemuxPipelineConfigFromToml,
      [output](pipeline::RemuxPipelineConfig config,
               const std::filesystem::path&) {
        *output = new MwRemux(std::move(config));
      });
}

MwResult mw_remux_on_status(MwRemux* remux, MwStatusCallback callback,
                            void* user_context) {
  return Guard([&]() {
    auto& handle = Require(remux, "Remux句柄");
    handle.pipeline.SetOnStatus([remux](auto status) {
      if (remux->on_status) {
        remux->on_status(ToStatus(status), remux->status_context);
      }
    });
    handle.on_status = callback;
    handle.status_context = user_context;
  });
}

MwResult mw_remux_start(MwRemux* remux) {
  return Guard([&]() { Require(remux, "Remux句柄").pipeline.Start(); });
}

MwResult mw_remux_status(const MwRemux* remux, MwPipelineStatus* output) {
  return Guard([&]() {
    Require(output, "状态输出") =
        ToStatus(Require(remux, "Remux句柄").pipeline.status());
  });
}

MwResult mw_remux_stats(MwRemux* remux, MwRemuxStats** output) {
  if (output) {
    *output = nullptr;
  }
  return Guard([&]() {
    auto& destination = Require(output, "性能统计输出");
    destination =
        MakeStats(Require(remux, "Remux句柄").pipeline.CollectPerformance());
  });
}

void mw_remux_stats_destroy(MwRemuxStats* stats) {
  ClearError();
  if (stats) {
    DestroyOutputs(stats->outputs, stats->output_count);
    delete stats;
  }
}

void mw_remux_stop(MwRemux* remux) {
  ClearError();
  if (remux) {
    remux->pipeline.Stop();
  } else {
    SetError("Remux句柄不能为空");
  }
}

void mw_remux_destroy(MwRemux* remux) {
  ClearError();
  delete remux;
}

MwResult mw_file_create(const char* config_path, MwFile** output) {
  if (output) {
    *output = nullptr;
  }
  if (!output) {
    return Guard([]() { throw std::invalid_argument("输出句柄不能为空"); });
  }
  return LoadConfig<pipeline::LocalFilePipelineConfig>(
      config_path, mw::streamer::config::LoadFilePipelineConfigFromToml,
      [output](pipeline::LocalFilePipelineConfig config,
               const std::filesystem::path& absolute_path) {
        *output = new MwFile(absolute_path, std::move(config));
      });
}

MwResult mw_file_on_status(MwFile* file, MwStatusCallback callback,
                           void* user_context) {
  return Guard([&]() {
    auto& handle = Require(file, "File句柄");
    handle.pipeline.SetOnStatus([file](auto status) {
      if (file->on_status) {
        file->on_status(ToStatus(status), file->status_context);
      }
    });
    handle.on_status = callback;
    handle.status_context = user_context;
  });
}

MwResult mw_file_set_processor(
    MwFile* file, const MwStreamerFileProcessorCallbacks* callbacks) {
  return Guard([&]() {
    Require(file, "File句柄")
        .pipeline.SetProcessorCallbacks(Require(callbacks, "Processor回调"));
  });
}

MwResult mw_file_start(MwFile* file) {
  return Guard([&]() { Require(file, "File句柄").pipeline.Start(); });
}

MwResult mw_file_reload(MwFile* file) {
  if (!file) {
    return Guard([]() { throw std::invalid_argument("File句柄不能为空"); });
  }
  return LoadConfig<pipeline::LocalFilePipelineConfig>(
      file->config_path, mw::streamer::config::LoadFilePipelineConfigFromToml,
      [file](pipeline::LocalFilePipelineConfig config,
             const std::filesystem::path&) {
        file->pipeline.UpdateProcessorConfig(
            std::move(config.processor.config));
      });
}

MwResult mw_file_status(const MwFile* file, MwPipelineStatus* output) {
  return Guard([&]() {
    Require(output, "状态输出") =
        ToStatus(Require(file, "File句柄").pipeline.status());
  });
}

MwResult mw_file_stats(MwFile* file, MwFileStats** output) {
  if (output) {
    *output = nullptr;
  }
  return Guard([&]() {
    auto& destination = Require(output, "性能统计输出");
    destination =
        MakeStats(Require(file, "File句柄").pipeline.CollectPerformance());
  });
}

void mw_file_stats_destroy(MwFileStats* stats) {
  ClearError();
  delete stats;
}

void mw_file_stop(MwFile* file) {
  ClearError();
  if (file) {
    file->pipeline.Stop();
  } else {
    SetError("File句柄不能为空");
  }
}

void mw_file_destroy(MwFile* file) {
  ClearError();
  delete file;
}

}  // extern "C"

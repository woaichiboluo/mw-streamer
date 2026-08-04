#include "mw/pipeline/file_pipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::pipeline::FilePipeline;
using mw::streamer::pipeline::FilePipelineStatus;
using mw::streamer::pipeline::LocalFilePipelineConfig;

std::filesystem::path SamplePath(const char* name) {
  return std::filesystem::path(MW_FILE_PIPELINE_TEST_DATA_DIR) / name;
}

LocalFilePipelineConfig MakeConfig(const char* name) {
  LocalFilePipelineConfig config;
  config.input_path = SamplePath(name).string();
  config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  return config;
}

void WaitForTerminalStatus(FilePipeline& pipeline,
                           std::condition_variable& condition,
                           std::mutex& mutex) {
  std::unique_lock<std::mutex> lock(mutex);
  REQUIRE(condition.wait_for(lock, 10s, [&pipeline]() {
    return pipeline.status() == FilePipelineStatus::kStopped ||
           pipeline.status() == FilePipelineStatus::kFailed;
  }));
}

struct AvCallbackState {
  std::atomic_size_t starts = 0;
  std::atomic_size_t video_frames = 0;
  std::atomic_size_t audio_frames = 0;
  std::atomic<std::uint64_t> audio_samples = 0;
  std::atomic_size_t boundaries = 0;
  std::atomic_size_t stops = 0;
  std::atomic_int active_callbacks = 0;
  std::atomic_bool callbacks_serial = true;
  std::atomic_bool source_info_valid = false;
  std::atomic_bool audio_format_valid = true;
  std::atomic<std::uint64_t> sequence = 0;
  std::atomic<std::uint64_t> start_sequence = 0;
  std::atomic<std::uint64_t> last_process_sequence = 0;
  std::atomic<std::uint64_t> boundary_sequence = 0;
  std::atomic<std::uint64_t> stop_sequence = 0;
  std::atomic<std::uint64_t> stopped_sequence = 0;
  std::mutex thread_mutex;
  std::thread::id worker_thread;
  bool one_worker = true;
};

void EnterCallback(AvCallbackState& state) {
  if (state.active_callbacks.fetch_add(1, std::memory_order_acq_rel) != 0) {
    state.callbacks_serial.store(false, std::memory_order_release);
  }
  std::lock_guard<std::mutex> lock(state.thread_mutex);
  if (state.worker_thread == std::thread::id{}) {
    state.worker_thread = std::this_thread::get_id();
  } else if (state.worker_thread != std::this_thread::get_id()) {
    state.one_worker = false;
  }
}

void LeaveCallback(AvCallbackState& state) {
  state.active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
}

MwStreamerFileProcessorCallbacks MakeAvCallbacks(AvCallbackState& state) {
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerFileProcessorStartRequest* request,
                          void* user_context) {
    auto& callback_state = *static_cast<AvCallbackState*>(user_context);
    EnterCallback(callback_state);
    callback_state.starts.fetch_add(1, std::memory_order_relaxed);
    callback_state.source_info_valid.store(
        request && request->source_info && request->source_info->has_audio &&
            request->source_info->has_video && request->execution &&
            request->execution->type == kMwStreamerExecutionCpu,
        std::memory_order_release);
    callback_state.start_sequence.store(
        callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
        std::memory_order_release);
    LeaveCallback(callback_state);
    return kMwStreamerProcessorStartSuccess;
  };
  callbacks.process_video = [](const MwStreamerVideoFrameView* frame,
                               void* user_context) {
    auto& callback_state = *static_cast<AvCallbackState*>(user_context);
    EnterCallback(callback_state);
    if (!frame || frame->buffer.memory_type != kMwStreamerMemoryHost ||
        frame->buffer.width == 0 || frame->buffer.height == 0) {
      callback_state.callbacks_serial.store(false, std::memory_order_release);
    }
    callback_state.video_frames.fetch_add(1, std::memory_order_relaxed);
    callback_state.last_process_sequence.store(
        callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
        std::memory_order_release);
    LeaveCallback(callback_state);
  };
  callbacks.process_audio = [](const MwStreamerAudioFrameView* frame,
                               void* user_context) {
    auto& callback_state = *static_cast<AvCallbackState*>(user_context);
    EnterCallback(callback_state);
    if (!frame || !frame->data || frame->sample_rate != 48000 ||
        frame->channel_count == 0 || frame->samples_per_channel == 0) {
      callback_state.audio_format_valid.store(false, std::memory_order_release);
    } else {
      callback_state.audio_samples.fetch_add(frame->samples_per_channel,
                                             std::memory_order_relaxed);
    }
    callback_state.audio_frames.fetch_add(1, std::memory_order_relaxed);
    callback_state.last_process_sequence.store(
        callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
        std::memory_order_release);
    LeaveCallback(callback_state);
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* user_context) {
    auto& callback_state = *static_cast<AvCallbackState*>(user_context);
    EnterCallback(callback_state);
    if (reason == kMwStreamerProcessorEndOfInput) {
      callback_state.boundaries.fetch_add(1, std::memory_order_relaxed);
      callback_state.boundary_sequence.store(
          callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
          std::memory_order_release);
    }
    LeaveCallback(callback_state);
  };
  callbacks.on_stop = [](void* user_context) {
    auto& callback_state = *static_cast<AvCallbackState*>(user_context);
    EnterCallback(callback_state);
    callback_state.stops.fetch_add(1, std::memory_order_relaxed);
    callback_state.stop_sequence.store(
        callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
        std::memory_order_release);
    LeaveCallback(callback_state);
  };
  return callbacks;
}

}  // namespace

TEST_CASE("FilePipeline全速处理音视频并报告进度") {
  AvCallbackState callback_state;
  std::mutex status_mutex;
  std::condition_variable condition;
  std::vector<FilePipelineStatus> statuses;
  FilePipeline pipeline(MakeConfig("h264_aac.mp4"));
  pipeline.SetProcessorCallbacks(MakeAvCallbacks(callback_state));
  pipeline.SetOnStatus([&](FilePipelineStatus status) {
    if (status == FilePipelineStatus::kStopped) {
      callback_state.stopped_sequence.store(
          callback_state.sequence.fetch_add(1, std::memory_order_acq_rel) + 1,
          std::memory_order_release);
    }
    {
      std::lock_guard<std::mutex> lock(status_mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  const auto started = std::chrono::steady_clock::now();
  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, status_mutex);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  REQUIRE(pipeline.status() == FilePipelineStatus::kStopped);
  CHECK(elapsed < 1500ms);
  CHECK(callback_state.starts.load() == 1);
  CHECK(callback_state.video_frames.load() == 20);
  CHECK(callback_state.audio_frames.load() == 94);
  CHECK(callback_state.audio_samples.load() == 96256);
  CHECK(callback_state.boundaries.load() == 1);
  CHECK(callback_state.stops.load() == 1);
  CHECK(callback_state.callbacks_serial.load());
  CHECK(callback_state.source_info_valid.load());
  CHECK(callback_state.audio_format_valid.load());
  {
    std::lock_guard<std::mutex> lock(callback_state.thread_mutex);
    CHECK(callback_state.one_worker);
  }
  CHECK(callback_state.start_sequence.load() <
        callback_state.last_process_sequence.load());
  CHECK(callback_state.last_process_sequence.load() <
        callback_state.boundary_sequence.load());
  CHECK(callback_state.boundary_sequence.load() <
        callback_state.stop_sequence.load());
  CHECK(callback_state.stop_sequence.load() <
        callback_state.stopped_sequence.load());

  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.interval > 0ns);
  CHECK(performance.has_audio);
  CHECK(performance.has_video);
  CHECK(performance.audio.decode.samples > 0);
  CHECK(performance.audio.process.samples == 96256);
  CHECK(performance.video.decode.frames == 20);
  CHECK(performance.video.process.frames == 20);
  CHECK(performance.progress_available);
  CHECK(performance.duration > 0us);
  CHECK(performance.processed_position == performance.duration);
  CHECK(performance.progress == 1.0);
  CHECK(performance.processing_speed_available);
  CHECK(performance.processing_speed > 1.0);

  const auto empty_interval = pipeline.CollectPerformance();
  CHECK(empty_interval.audio.decode.samples == 0);
  CHECK(empty_interval.audio.process.samples == 0);
  CHECK(empty_interval.video.decode.frames == 0);
  CHECK(empty_interval.video.process.frames == 0);
  CHECK(empty_interval.progress == 1.0);
  CHECK(empty_interval.processing_speed == 0.0);

  pipeline.Stop();
  pipeline.Stop();
  CHECK(callback_state.stops.load() == 1);
  {
    std::lock_guard<std::mutex> lock(status_mutex);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == FilePipelineStatus::kStarting);
    CHECK(statuses[1] == FilePipelineStatus::kRunning);
    CHECK(statuses[2] == FilePipelineStatus::kStopped);
  }
}

TEST_CASE("FilePipeline自然结束时排空B帧") {
  std::atomic_size_t video_frames = 0;
  std::mutex mutex;
  std::condition_variable condition;
  FilePipeline pipeline(MakeConfig("h265_aac.mp4"));
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &video_frames;
  callbacks.process_video = [](const MwStreamerVideoFrameView*,
                               void* user_context) {
    static_cast<std::atomic_size_t*>(user_context)->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](FilePipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == FilePipelineStatus::kStopped);
  CHECK(video_frames.load() == 20);
  pipeline.Stop();
}

TEST_CASE("FilePipeline支持纯视频文件") {
  std::atomic_size_t video_frames = 0;
  std::atomic_size_t audio_frames = 0;
  std::mutex mutex;
  std::condition_variable condition;
  struct CallbackState {
    std::atomic_size_t* video_frames;
    std::atomic_size_t* audio_frames;
  } callback_state{&video_frames, &audio_frames};
  FilePipeline pipeline(MakeConfig("h264_video.mp4"));
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.process_video = [](const MwStreamerVideoFrameView*,
                               void* user_context) {
    static_cast<CallbackState*>(user_context)->video_frames->fetch_add(1);
  };
  callbacks.process_audio = [](const MwStreamerAudioFrameView*,
                               void* user_context) {
    static_cast<CallbackState*>(user_context)->audio_frames->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](FilePipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == FilePipelineStatus::kStopped);
  CHECK(video_frames.load() == 20);
  CHECK(audio_frames.load() == 0);
  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.has_video);
  CHECK_FALSE(performance.has_audio);
  CHECK(performance.progress == 1.0);
  pipeline.Stop();
}

TEST_CASE("FilePipeline主动停止等待当前Processor回调且不通知文件结束") {
  struct CallbackState {
    std::mutex mutex;
    std::condition_variable entered_condition;
    std::condition_variable release_condition;
    bool entered = false;
    bool released = false;
    std::atomic_size_t process_calls = 0;
    std::atomic_size_t boundaries = 0;
    std::atomic_size_t stops = 0;
  } callback_state;
  FilePipeline pipeline(MakeConfig("packet_queue_8s.mp4"));
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.process_video = [](const MwStreamerVideoFrameView*,
                               void* user_context) {
    auto& state = *static_cast<CallbackState*>(user_context);
    state.process_calls.fetch_add(1);
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.entered) {
      state.entered = true;
      state.entered_condition.notify_all();
      state.release_condition.wait(lock, [&state]() { return state.released; });
    }
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason,
                             void* user_context) {
    static_cast<CallbackState*>(user_context)->boundaries.fetch_add(1);
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<CallbackState*>(user_context)->stops.fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.Start();
  {
    std::unique_lock<std::mutex> lock(callback_state.mutex);
    REQUIRE(callback_state.entered_condition.wait_for(
        lock, 10s, [&callback_state]() { return callback_state.entered; }));
  }

  std::atomic_bool stop_returned = false;
  std::thread stopper([&]() {
    pipeline.Stop();
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(stop_returned.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    callback_state.released = true;
  }
  callback_state.release_condition.notify_all();
  stopper.join();

  CHECK(stop_returned.load(std::memory_order_acquire));
  CHECK(pipeline.status() == FilePipelineStatus::kStopped);
  CHECK(callback_state.process_calls.load() == 1);
  CHECK(callback_state.boundaries.load() == 0);
  CHECK(callback_state.stops.load() == 1);
}

TEST_CASE("FilePipeline异步报告文件打开和Processor启动失败") {
  SECTION("文件不存在") {
    auto config = MakeConfig("missing-file.mp4");
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_size_t starts = 0;
    FilePipeline pipeline(std::move(config));
    MwStreamerFileProcessorCallbacks callbacks{};
    callbacks.user_context = &starts;
    callbacks.on_start = [](const MwStreamerFileProcessorStartRequest*,
                            void* user_context) {
      static_cast<std::atomic_size_t*>(user_context)->fetch_add(1);
      return kMwStreamerProcessorStartSuccess;
    };
    pipeline.SetProcessorCallbacks(callbacks);
    pipeline.SetOnStatus([&](FilePipelineStatus) { condition.notify_all(); });

    pipeline.Start();
    WaitForTerminalStatus(pipeline, condition, mutex);
    CHECK(pipeline.status() == FilePipelineStatus::kFailed);
    CHECK(starts.load() == 0);
    pipeline.Stop();
    CHECK(pipeline.status() == FilePipelineStatus::kFailed);
  }

  SECTION("Processor拒绝启动") {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_size_t stops = 0;
    FilePipeline pipeline(MakeConfig("h264_video.mp4"));
    MwStreamerFileProcessorCallbacks callbacks{};
    callbacks.user_context = &stops;
    callbacks.on_start = [](const MwStreamerFileProcessorStartRequest*, void*) {
      return kMwStreamerProcessorStartFailed;
    };
    callbacks.on_stop = [](void* user_context) {
      static_cast<std::atomic_size_t*>(user_context)->fetch_add(1);
    };
    pipeline.SetProcessorCallbacks(callbacks);
    pipeline.SetOnStatus([&](FilePipelineStatus) { condition.notify_all(); });

    pipeline.Start();
    WaitForTerminalStatus(pipeline, condition, mutex);
    CHECK(pipeline.status() == FilePipelineStatus::kFailed);
    CHECK(stops.load() == 0);
    pipeline.Stop();
  }
}

TEST_CASE("FilePipeline将Processor运行异常转为失败并停止Processor") {
  struct CallbackState {
    std::atomic_size_t boundaries = 0;
    std::atomic_size_t stops = 0;
  } callback_state;
  std::mutex mutex;
  std::condition_variable condition;
  FilePipeline pipeline(MakeConfig("h264_video.mp4"));
  MwStreamerFileProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.process_video = [](const MwStreamerVideoFrameView*, void*) {
    throw std::runtime_error("业务处理失败");
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason,
                             void* user_context) {
    static_cast<CallbackState*>(user_context)->boundaries.fetch_add(1);
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<CallbackState*>(user_context)->stops.fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](FilePipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == FilePipelineStatus::kFailed);
  CHECK(callback_state.boundaries.load() == 0);
  CHECK(callback_state.stops.load() == 1);
  pipeline.Stop();
  CHECK(pipeline.status() == FilePipelineStatus::kFailed);
  CHECK(callback_state.stops.load() == 1);
}

TEST_CASE("FilePipeline同步拒绝空文件路径") {
  LocalFilePipelineConfig config;
  FilePipeline pipeline(std::move(config));

  CHECK_NOTHROW(pipeline.UpdateProcessorConfig("initial"));
  CHECK_THROWS_AS(pipeline.Start(), std::invalid_argument);
  CHECK(pipeline.status() == FilePipelineStatus::kIdle);
}

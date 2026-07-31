#include "mw/pipeline/streaming_pipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "Record/MP4Demuxer.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::pipeline::StreamingPipeline;
using mw::streamer::pipeline::StreamingPipelineConfig;
using mw::streamer::pipeline::StreamingPipelineStatus;

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-pipeline-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

std::filesystem::path SamplePath(const char* name) {
  return std::filesystem::path(MW_STREAMING_PIPELINE_TEST_DATA_DIR) / name;
}

std::filesystem::path FindRecordedMp4(const std::filesystem::path& directory) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
      return entry.path();
    }
  }
  return {};
}

void WaitForTerminalStatus(StreamingPipeline& pipeline,
                           std::condition_variable& condition,
                           std::mutex& mutex) {
  std::unique_lock<std::mutex> lock(mutex);
  REQUIRE(condition.wait_for(lock, 10s, [&]() {
    return pipeline.status() == StreamingPipelineStatus::kStopped ||
           pipeline.status() == StreamingPipelineStatus::kFailed;
  }));
}

StreamingPipelineConfig MakeSoftwareConfig(
    const std::filesystem::path& input, const std::filesystem::path& output) {
  StreamingPipelineConfig config;
  config.input_url = input.string();
  config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  config.processor.output_width = 64;
  config.processor.output_height = 64;
  config.video_encoder.frame_rate = {10, 1};
  config.video_encoder.properties["tune"] = "zerolatency";
  config.output_targets = {output.string()};
  return config;
}

}  // namespace

TEST_CASE("StreamingPipeline完成音视频处理并生成fMP4") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_aac.mp4"),
                                   directory.path() / "result.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<StreamingPipelineStatus> statuses;
  std::atomic_size_t end_of_input_calls = 0;
  std::atomic_size_t stop_calls = 0;
  struct CallbackState {
    std::atomic_size_t* end_of_input_calls;
    std::atomic_size_t* stop_calls;
  } callback_state{&end_of_input_calls, &stop_calls};
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* user_context) {
    if (reason == kMwStreamerProcessorEndOfInput) {
      static_cast<CallbackState*>(user_context)
          ->end_of_input_calls->fetch_add(1);
    }
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<CallbackState*>(user_context)->stop_calls->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  CHECK(end_of_input_calls.load() == 1);
  CHECK(stop_calls.load() == 0);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(stop_calls.load() == 1);
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == StreamingPipelineStatus::kStarting);
    CHECK(statuses[1] == StreamingPipelineStatus::kRunning);
    CHECK(statuses[2] == StreamingPipelineStatus::kStopped);
  }

  const auto output_path = FindRecordedMp4(directory.path());
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  CHECK(demuxer.getTracks(true).size() == 2);
  CHECK(demuxer.getDurationMS() >= 1800);
}

TEST_CASE("StreamingPipeline支持纯视频处理") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "video.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kStopped);
  pipeline.Stop();

  const auto output_path = FindRecordedMp4(directory.path());
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE(tracks.size() == 1);
  CHECK(tracks.front()->getTrackType() == mediakit::TrackVideo);
}

TEST_CASE("StreamingPipeline同步拒绝无效配置") {
  StreamingPipelineConfig config;
  StreamingPipeline pipeline(std::move(config));

  CHECK_NOTHROW(pipeline.UpdateProcessorConfig("updated"));
  CHECK_THROWS_AS(pipeline.Start(), std::invalid_argument);
  CHECK(pipeline.status() == StreamingPipelineStatus::kIdle);
}

TEST_CASE("StreamingPipeline保留Processor启动失败状态") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "failed.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.on_start = [](const MwStreamerStreamingProcessorStartRequest*,
                          void*) -> MwStreamerProcessorStartResult {
    return kMwStreamerProcessorStartFailed;
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kFailed);
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kFailed);
}

TEST_CASE("StreamingPipeline消化运行期异常并等待外部停止") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "failed-runtime.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<StreamingPipelineStatus> statuses;
  std::atomic_size_t stop_calls = 0;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &stop_calls;
  callbacks.process_video = [](const MwStreamerStreamingVideoProcessRequest*,
                               void*) {
    throw std::runtime_error("业务处理失败");
  };
  callbacks.on_stop = [](void* user_context) {
    static_cast<std::atomic_size_t*>(user_context)->fetch_add(1);
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus([&](StreamingPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kFailed);
  CHECK(stop_calls.load() == 0);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kFailed);
  CHECK(stop_calls.load() == 1);
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE_FALSE(statuses.empty());
    CHECK(statuses.back() == StreamingPipelineStatus::kFailed);
  }
}

TEST_CASE("StreamingPipeline可在异步初始化期间立即停止") {
  TestDirectory directory;
  for (int iteration = 0; iteration < 10; ++iteration) {
    auto config = MakeSoftwareConfig(
        SamplePath("h264_video.mp4"),
        directory.path() / ("stopped-" + std::to_string(iteration) + ".mp4"));
    StreamingPipeline pipeline(std::move(config));

    pipeline.Start();
    pipeline.Stop();
    CHECK(pipeline.status() == StreamingPipelineStatus::kStopped);
  }
}

TEST_CASE("StreamingPipeline支持外部线程更新Processor配置并停止") {
  TestDirectory directory;
  auto config = MakeSoftwareConfig(SamplePath("h264_video.mp4"),
                                   directory.path() / "updated.mp4");
  std::mutex mutex;
  std::condition_variable condition;
  struct CallbackState {
    std::mutex mutex;
    std::string initial_config;
    std::string updated_config;
    std::size_t update_calls = 0;
  } callback_state;
  StreamingPipeline pipeline(std::move(config));
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &callback_state;
  callbacks.on_start =
      [](const MwStreamerStreamingProcessorStartRequest* request,
         void* user_context) {
        auto& state = *static_cast<CallbackState*>(user_context);
        std::lock_guard<std::mutex> lock(state.mutex);
        state.initial_config = request->config->config;
        return kMwStreamerProcessorStartSuccess;
      };
  callbacks.update_config = [](const char* updated_config, void* user_context) {
    auto& state = *static_cast<CallbackState*>(user_context);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.updated_config = updated_config;
    ++state.update_calls;
  };
  pipeline.SetProcessorCallbacks(callbacks);
  pipeline.SetOnStatus(
      [&](StreamingPipelineStatus) { condition.notify_all(); });

  pipeline.UpdateProcessorConfig("initial");
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.update_calls == 0);
  }
  pipeline.Start();
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(condition.wait_for(lock, 10s, [&pipeline]() {
      return pipeline.status() == StreamingPipelineStatus::kRunning ||
             pipeline.status() == StreamingPipelineStatus::kFailed;
    }));
  }
  REQUIRE(pipeline.status() == StreamingPipelineStatus::kRunning);
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.initial_config == "initial");
    CHECK(callback_state.update_calls == 0);
  }
  pipeline.UpdateProcessorConfig("updated");
  {
    std::lock_guard<std::mutex> lock(callback_state.mutex);
    CHECK(callback_state.updated_config == "updated");
    CHECK(callback_state.update_calls == 1);
  }
  pipeline.Stop();
  CHECK(pipeline.status() == StreamingPipelineStatus::kStopped);
  CHECK_THROWS_AS(pipeline.UpdateProcessorConfig("stopped"), std::logic_error);
}

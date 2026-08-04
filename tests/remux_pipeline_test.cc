#include "mw/pipeline/remux_pipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::pipeline::RemuxPipeline;
using mw::streamer::pipeline::RemuxPipelineConfig;
using mw::streamer::pipeline::RemuxPipelineStatus;

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-remux-pipeline-" + std::to_string(suffix));
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

std::filesystem::path SamplePath() {
  return std::filesystem::path(MW_REMUX_PIPELINE_TEST_DATA_DIR) /
         "h264_aac.mp4";
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

void WaitForTerminalStatus(RemuxPipeline& pipeline,
                           std::condition_variable& condition,
                           std::mutex& mutex) {
  std::unique_lock<std::mutex> lock(mutex);
  REQUIRE(condition.wait_for(lock, 10s, [&]() {
    return pipeline.status() == RemuxPipelineStatus::kStopped ||
           pipeline.status() == RemuxPipelineStatus::kFailed;
  }));
}

}  // namespace

TEST_CASE("RemuxPipeline不解码重编码并生成源规格fMP4") {
  TestDirectory directory;
  RemuxPipelineConfig config;
  config.input_url = SamplePath().string();
  config.output_targets = {
      (directory.path() / "nested" / "source.mp4").string()};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<RemuxPipelineStatus> statuses;
  RemuxPipeline pipeline(std::move(config));
  pipeline.SetOnStatus([&](RemuxPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == RemuxPipelineStatus::kStopped);

  const auto performance = pipeline.CollectPerformance();
  CHECK(performance.interval > 0ns);
  CHECK_FALSE(performance.input.is_network);
  CHECK_FALSE(performance.input.connected);
  CHECK(performance.input.generation == 1);
  CHECK(performance.input.reconnect_count == 0);
  CHECK(performance.input.received_bytes == 0);
  CHECK(performance.packets == 115);
  CHECK(performance.bytes > 0);
  CHECK(performance.bits_per_second ==
        Catch::Approx(
            static_cast<double>(performance.bytes) * 8.0 /
            std::chrono::duration<double>(performance.interval).count()));
  CHECK(performance.output_queue_depth == 0);
  CHECK(performance.outputs.empty());
  const auto empty_performance = pipeline.CollectPerformance();
  CHECK(empty_performance.interval > 0ns);
  CHECK_FALSE(empty_performance.input.is_network);
  CHECK_FALSE(empty_performance.input.connected);
  CHECK(empty_performance.input.generation == 1);
  CHECK(empty_performance.input.reconnect_count == 0);
  CHECK(empty_performance.input.received_bytes == 0);
  CHECK(empty_performance.packets == 0);
  CHECK(empty_performance.bytes == 0);
  CHECK(empty_performance.bits_per_second == 0.0);
  CHECK(empty_performance.output_queue_depth == 0);
  CHECK(empty_performance.outputs.empty());

  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(statuses.size() == 3);
    CHECK(statuses[0] == RemuxPipelineStatus::kStarting);
    CHECK(statuses[1] == RemuxPipelineStatus::kRunning);
    CHECK(statuses[2] == RemuxPipelineStatus::kStopped);
  }

  const auto output_path = FindRecordedMp4(directory.path() / "nested");
  REQUIRE_FALSE(output_path.empty());
  mediakit::MP4Demuxer demuxer;
  demuxer.openMP4(output_path.string());
  const auto tracks = demuxer.getTracks(true);
  REQUIRE(tracks.size() == 2);
  CHECK(demuxer.getDurationMS() >= 1800);
  bool found_video = false;
  bool found_audio = false;
  for (const auto& track : tracks) {
    if (track->getTrackType() == mediakit::TrackVideo) {
      const auto video = std::dynamic_pointer_cast<mediakit::VideoTrack>(track);
      REQUIRE(video);
      CHECK(video->getCodecId() == mediakit::CodecH264);
      CHECK(video->getVideoWidth() == 64);
      CHECK(video->getVideoHeight() == 64);
      found_video = true;
    } else if (track->getTrackType() == mediakit::TrackAudio) {
      CHECK(track->getCodecId() == mediakit::CodecAAC);
      found_audio = true;
    }
  }
  CHECK(found_video);
  CHECK(found_audio);

  pipeline.Stop();
  pipeline.Stop();
}

TEST_CASE("RemuxPipeline在全部输出目标永久失效后失败") {
  TestDirectory directory;
  const auto blocked_path = directory.path() / "blocked";
  std::ofstream(blocked_path).put('x');
  RemuxPipelineConfig config;
  config.input_url = SamplePath().string();
  config.output_targets = {(blocked_path / "source.mp4").string()};
  std::mutex mutex;
  std::condition_variable condition;
  RemuxPipeline pipeline(std::move(config));
  pipeline.SetOnStatus([&](RemuxPipelineStatus) { condition.notify_all(); });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == RemuxPipelineStatus::kFailed);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.status() == RemuxPipelineStatus::kFailed);
  CHECK_FALSE(std::filesystem::is_directory(blocked_path));
}

TEST_CASE("RemuxPipeline同步拒绝空输入和空输出") {
  RemuxPipelineConfig empty;
  RemuxPipeline empty_pipeline(std::move(empty));
  CHECK_THROWS_AS(empty_pipeline.Start(), std::invalid_argument);
  CHECK(empty_pipeline.status() == RemuxPipelineStatus::kIdle);

  RemuxPipelineConfig no_output;
  no_output.input_url = SamplePath().string();
  RemuxPipeline no_output_pipeline(std::move(no_output));
  CHECK_THROWS_AS(no_output_pipeline.Start(), std::invalid_argument);
  CHECK(no_output_pipeline.status() == RemuxPipelineStatus::kIdle);
}

TEST_CASE("RemuxPipeline输入源失败后保持失败状态") {
  TestDirectory directory;
  RemuxPipelineConfig config;
  config.input_url = (directory.path() / "missing.mp4").string();
  config.output_targets = {(directory.path() / "output.mp4").string()};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<RemuxPipelineStatus> statuses;
  RemuxPipeline pipeline(std::move(config));
  pipeline.SetOnStatus([&](RemuxPipelineStatus status) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      statuses.push_back(status);
    }
    condition.notify_all();
  });

  pipeline.Start();
  WaitForTerminalStatus(pipeline, condition, mutex);
  REQUIRE(pipeline.status() == RemuxPipelineStatus::kFailed);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.status() == RemuxPipelineStatus::kFailed);
  {
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(statuses.size() == 2);
    CHECK(statuses[0] == RemuxPipelineStatus::kStarting);
    CHECK(statuses[1] == RemuxPipelineStatus::kFailed);
  }
  CHECK(FindRecordedMp4(directory.path()).empty());
}

TEST_CASE("RemuxPipeline可在异步启动期间立即停止") {
  TestDirectory directory;
  for (int iteration = 0; iteration < 5; ++iteration) {
    RemuxPipelineConfig config;
    config.input_url = SamplePath().string();
    config.output_targets = {
        (directory.path() / ("stopped-" + std::to_string(iteration) + ".mp4"))
            .string()};
    RemuxPipeline pipeline(std::move(config));

    pipeline.Start();
    pipeline.Stop();
    CHECK(pipeline.status() == RemuxPipelineStatus::kStopped);
    pipeline.Stop();
    CHECK(pipeline.status() == RemuxPipelineStatus::kStopped);
  }
}

TEST_CASE("RemuxPipeline性能采集可与外部停止并发") {
  TestDirectory directory;
  RemuxPipelineConfig config;
  config.input_url = SamplePath().string();
  config.output_targets = {(directory.path() / "collected.mp4").string()};
  RemuxPipeline pipeline(std::move(config));
  std::atomic_bool collecting = true;
  std::atomic_bool collection_attempted = false;
  std::atomic<std::uint64_t> collection_count = 0;
  std::exception_ptr collection_error;

  pipeline.Start();
  std::thread collector([&]() {
    try {
      while (collecting.load(std::memory_order_acquire)) {
        static_cast<void>(pipeline.CollectPerformance());
        collection_count.fetch_add(1, std::memory_order_relaxed);
        collection_attempted.store(true, std::memory_order_release);
      }
    } catch (...) {
      collection_error = std::current_exception();
      collection_attempted.store(true, std::memory_order_release);
    }
  });
  while (!collection_attempted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  pipeline.Stop();
  collecting.store(false, std::memory_order_release);
  collector.join();

  if (collection_error) {
    std::rethrow_exception(collection_error);
  }
  CHECK(collection_count.load(std::memory_order_relaxed) > 0);
  CHECK(pipeline.status() == RemuxPipelineStatus::kStopped);
  static_cast<void>(pipeline.CollectPerformance());
}

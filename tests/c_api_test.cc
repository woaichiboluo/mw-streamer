#include "mw/c_api.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TestDirectory final {
 public:
  TestDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-c-api-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path Write(const char* name,
                              const std::string& content) const {
    const auto path = path_ / name;
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(output);
    return path;
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

class CurrentPathGuard final {
 public:
  explicit CurrentPathGuard(const std::filesystem::path& path)
      : original_path_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }

  ~CurrentPathGuard() {
    std::error_code error;
    std::filesystem::current_path(original_path_, error);
  }

 private:
  std::filesystem::path original_path_;
};

struct StatusState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<MwPipelineStatus> statuses;
};

void CheckNaturalStatuses(StatusState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  REQUIRE(state.statuses.size() == 3);
  CHECK(state.statuses[0] == kMwPipelineStatusStarting);
  CHECK(state.statuses[1] == kMwPipelineStatusRunning);
  CHECK(state.statuses[2] == kMwPipelineStatusStopped);
}

void OnStatus(MwPipelineStatus status, void* user_context) {
  auto& state = *static_cast<StatusState*>(user_context);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.statuses.push_back(status);
  }
  state.condition.notify_all();
}

template <typename Handle, typename StatusFunction>
void WaitForTerminal(Handle* handle, StatusState& state,
                     StatusFunction status_function) {
  std::unique_lock<std::mutex> lock(state.mutex);
  REQUIRE(state.condition.wait_for(lock, 10s, [&]() {
    MwPipelineStatus status = kMwPipelineStatusIdle;
    REQUIRE(status_function(handle, &status) == kMwResultSuccess);
    return status == kMwPipelineStatusStopped ||
           status == kMwPipelineStatusFailed;
  }));
}

struct ProcessorState {
  std::atomic_size_t starts = 0;
  std::atomic_size_t updates = 0;
  std::mutex mutex;
  std::string config;
  std::string updated_config;
};

MwStreamerProcessorStartResult OnStreamingProcessorStart(
    const MwStreamerStreamingProcessorStartRequest* request,
    void* user_context) {
  auto& state = *static_cast<ProcessorState*>(user_context);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.config = request->config->config;
  }
  state.starts.fetch_add(1, std::memory_order_relaxed);
  return kMwStreamerProcessorStartSuccess;
}

MwStreamerProcessorStartResult OnFileProcessorStart(
    const MwStreamerFileProcessorStartRequest* request, void* user_context) {
  auto& state = *static_cast<ProcessorState*>(user_context);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.config = request->config->config;
  }
  state.starts.fetch_add(1, std::memory_order_relaxed);
  return kMwStreamerProcessorStartSuccess;
}

void OnProcessorConfigUpdate(const char* config, void* user_context) {
  auto& state = *static_cast<ProcessorState*>(user_context);
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.updated_config = config;
  }
  state.updates.fetch_add(1, std::memory_order_relaxed);
}

std::filesystem::path SamplePath() {
  return std::filesystem::path(MW_C_API_TEST_DATA_DIR) / "h264_aac.mp4";
}

}  // namespace

TEST_CASE("C API从TOML运行三类Pipeline并导出状态与统计") {
  TestDirectory directory;

  MwStreaming* invalid = nullptr;
  CHECK(mw_streaming_create("missing.toml", &invalid) == kMwResultConfigError);
  CHECK(invalid == nullptr);
  CHECK(std::string(mw_last_error()).find("missing.toml") != std::string::npos);

  REQUIRE(mw_init(nullptr) == kMwResultSuccess);
  struct ShutdownGuard {
    ~ShutdownGuard() { mw_shutdown(); }
  } shutdown_guard;

  {
    const auto MakeStreamingConfig = [](std::string_view mode) {
      return fmt::format(R"(input_url = "{}"
output_targets = []

[video_decoder]
backend = "software"

[processor]
output_width = 64
output_height = 64

[processor.config]
mode = "{}"

[video_encoder]
frame_rate = {{ num = 10, den = 1 }}

[video_encoder.properties]
tune = "zerolatency"
)",
                         SamplePath().string(), mode);
    };
    const auto config_path =
        directory.Write("streaming.toml", MakeStreamingConfig("preview"));
    MwStreaming* raw = nullptr;
    REQUIRE(mw_streaming_create(config_path.c_str(), &raw) == kMwResultSuccess);
    const auto destroy = [](MwStreaming* value) {
      mw_streaming_destroy(value);
    };
    std::unique_ptr<MwStreaming, decltype(destroy)> streaming(raw, destroy);

    StatusState status_state;
    REQUIRE(mw_streaming_on_status(streaming.get(), OnStatus, &status_state) ==
            kMwResultSuccess);
    ProcessorState processor_state;
    MwStreamerStreamingProcessorCallbacks callbacks{};
    callbacks.user_context = &processor_state;
    callbacks.on_start = OnStreamingProcessorStart;
    callbacks.update_config = OnProcessorConfigUpdate;
    REQUIRE(mw_streaming_set_processor(streaming.get(), &callbacks) ==
            kMwResultSuccess);

    REQUIRE(mw_streaming_start(streaming.get()) == kMwResultSuccess);
    {
      std::unique_lock<std::mutex> lock(status_state.mutex);
      REQUIRE(status_state.condition.wait_for(lock, 10s, [&]() {
        return std::find(
                   status_state.statuses.begin(), status_state.statuses.end(),
                   kMwPipelineStatusRunning) != status_state.statuses.end();
      }));
    }
    directory.Write("streaming.toml", MakeStreamingConfig("auto_ptz"));
    REQUIRE(mw_streaming_reload(streaming.get()) == kMwResultSuccess);
    CHECK(processor_state.updates.load(std::memory_order_relaxed) == 1);
    {
      std::lock_guard<std::mutex> lock(processor_state.mutex);
      CHECK(processor_state.updated_config.find("mode = 'auto_ptz'") !=
            std::string::npos);
      CHECK(processor_state.updated_config.find("input_url") ==
            std::string::npos);
    }

    directory.Write("streaming.toml", "[processor.config]\nmode = [\n");
    REQUIRE(mw_streaming_reload(streaming.get()) == kMwResultConfigError);
    CHECK(processor_state.updates.load(std::memory_order_relaxed) == 1);
    {
      std::lock_guard<std::mutex> lock(processor_state.mutex);
      CHECK(processor_state.updated_config.find("mode = 'auto_ptz'") !=
            std::string::npos);
    }

    WaitForTerminal(streaming.get(), status_state, mw_streaming_status);
    MwPipelineStatus status = kMwPipelineStatusIdle;
    REQUIRE(mw_streaming_status(streaming.get(), &status) == kMwResultSuccess);
    REQUIRE(status == kMwPipelineStatusStopped);
    CheckNaturalStatuses(status_state);
    CHECK(processor_state.starts.load() == 1);
    {
      std::lock_guard<std::mutex> lock(processor_state.mutex);
      CHECK(processor_state.config.find("mode = 'preview'") !=
            std::string::npos);
    }

    MwStreamingStats* stats = nullptr;
    REQUIRE(mw_streaming_stats(streaming.get(), &stats) == kMwResultSuccess);
    REQUIRE(stats != nullptr);
    CHECK(stats->has_video == 1);
    CHECK(stats->has_audio == 1);
    CHECK(stats->video.encode.frames > 0);
    CHECK(stats->audio.encode.samples > 0);
    mw_streaming_stats_destroy(stats);
    directory.Write("streaming.toml", MakeStreamingConfig("stopped"));
    CHECK(mw_streaming_reload(streaming.get()) == kMwResultInvalidState);
    CHECK(mw_streaming_on_status(streaming.get(), OnStatus, &status_state) ==
          kMwResultInvalidState);
    CHECK_FALSE(std::string(mw_last_error()).empty());
    mw_streaming_stop(streaming.get());
  }

  {
    const auto config_path = directory.Write(
        "remux.toml",
        fmt::format("input_url = \"{}\"\noutput_targets = [\"{}\"]\n",
                    SamplePath().string(),
                    (directory.path() / "source.mp4").string()));
    MwRemux* raw = nullptr;
    REQUIRE(mw_remux_create(config_path.c_str(), &raw) == kMwResultSuccess);
    const auto destroy = [](MwRemux* value) { mw_remux_destroy(value); };
    std::unique_ptr<MwRemux, decltype(destroy)> remux(raw, destroy);
    StatusState status_state;
    REQUIRE(mw_remux_on_status(remux.get(), OnStatus, &status_state) ==
            kMwResultSuccess);
    REQUIRE(mw_remux_start(remux.get()) == kMwResultSuccess);
    WaitForTerminal(remux.get(), status_state, mw_remux_status);

    MwPipelineStatus status = kMwPipelineStatusIdle;
    REQUIRE(mw_remux_status(remux.get(), &status) == kMwResultSuccess);
    REQUIRE(status == kMwPipelineStatusStopped);
    CheckNaturalStatuses(status_state);
    MwRemuxStats* stats = nullptr;
    REQUIRE(mw_remux_stats(remux.get(), &stats) == kMwResultSuccess);
    REQUIRE(stats != nullptr);
    CHECK(stats->packets > 0);
    CHECK(stats->bytes > 0);
    mw_remux_stats_destroy(stats);
    mw_remux_stop(remux.get());
  }

  {
    const auto MakeFileConfig = [](std::string_view task) {
      return fmt::format(R"(input_path = "{}"

[video_decoder]
backend = "software"

[processor.config]
task = "{}"
)",
                         SamplePath().string(), task);
    };
    directory.Write("file.toml", MakeFileConfig("offline"));
    MwFile* raw = nullptr;
    {
      CurrentPathGuard current_path(directory.path());
      REQUIRE(mw_file_create("file.toml", &raw) == kMwResultSuccess);
    }
    const auto destroy = [](MwFile* value) { mw_file_destroy(value); };
    std::unique_ptr<MwFile, decltype(destroy)> file(raw, destroy);
    StatusState status_state;
    REQUIRE(mw_file_on_status(file.get(), OnStatus, &status_state) ==
            kMwResultSuccess);
    ProcessorState processor_state;
    MwStreamerFileProcessorCallbacks callbacks{};
    callbacks.user_context = &processor_state;
    callbacks.on_start = OnFileProcessorStart;
    callbacks.update_config = OnProcessorConfigUpdate;
    REQUIRE(mw_file_set_processor(file.get(), &callbacks) == kMwResultSuccess);

    directory.Write("file.toml", MakeFileConfig("updated_offline"));
    REQUIRE(mw_file_reload(file.get()) == kMwResultSuccess);
    directory.Write("file.toml", "[processor.config]\ntask = [\n");
    REQUIRE(mw_file_reload(file.get()) == kMwResultConfigError);
    CHECK(processor_state.updates.load(std::memory_order_relaxed) == 0);

    REQUIRE(mw_file_start(file.get()) == kMwResultSuccess);
    WaitForTerminal(file.get(), status_state, mw_file_status);

    MwPipelineStatus status = kMwPipelineStatusIdle;
    REQUIRE(mw_file_status(file.get(), &status) == kMwResultSuccess);
    REQUIRE(status == kMwPipelineStatusStopped);
    CheckNaturalStatuses(status_state);
    CHECK(processor_state.starts.load() == 1);
    {
      std::lock_guard<std::mutex> lock(processor_state.mutex);
      CHECK(processor_state.config.find("task = 'updated_offline'") !=
            std::string::npos);
      CHECK(processor_state.config.find("input_path") == std::string::npos);
    }
    MwFileStats* stats = nullptr;
    REQUIRE(mw_file_stats(file.get(), &stats) == kMwResultSuccess);
    REQUIRE(stats != nullptr);
    CHECK(stats->progress_available == 1);
    CHECK(stats->progress == 1.0);
    CHECK(stats->video.process.frames > 0);
    mw_file_stats_destroy(stats);
    directory.Write("file.toml", MakeFileConfig("stopped"));
    CHECK(mw_file_reload(file.get()) == kMwResultInvalidState);
    mw_file_stop(file.get());
  }

  const auto streaming_config =
      directory.Write("destroy_streaming.toml",
                      fmt::format("input_url = \"{}\"\noutput_targets = []\n\n"
                                  "[video_decoder]\nbackend = \"software\"\n",
                                  SamplePath().string()));
  StatusState streaming_status;
  MwStreaming* streaming = nullptr;
  REQUIRE(mw_streaming_create(streaming_config.c_str(), &streaming) ==
          kMwResultSuccess);
  REQUIRE(mw_streaming_on_status(streaming, OnStatus, &streaming_status) ==
          kMwResultSuccess);
  REQUIRE(mw_streaming_start(streaming) == kMwResultSuccess);
  mw_streaming_destroy(streaming);

  const auto remux_config = directory.Write(
      "destroy_remux.toml",
      fmt::format("input_url = \"{}\"\noutput_targets = [\"{}\"]\n",
                  SamplePath().string(),
                  (directory.path() / "destroy_source.mp4").string()));
  StatusState remux_status;
  MwRemux* remux = nullptr;
  REQUIRE(mw_remux_create(remux_config.c_str(), &remux) == kMwResultSuccess);
  REQUIRE(mw_remux_on_status(remux, OnStatus, &remux_status) ==
          kMwResultSuccess);
  REQUIRE(mw_remux_start(remux) == kMwResultSuccess);
  mw_remux_destroy(remux);

  const auto file_config =
      directory.Write("destroy_file.toml",
                      fmt::format("input_path = \"{}\"\n\n"
                                  "[video_decoder]\nbackend = \"software\"\n",
                                  SamplePath().string()));
  StatusState file_status;
  MwFile* file = nullptr;
  REQUIRE(mw_file_create(file_config.c_str(), &file) == kMwResultSuccess);
  REQUIRE(mw_file_on_status(file, OnStatus, &file_status) == kMwResultSuccess);
  REQUIRE(mw_file_start(file) == kMwResultSuccess);
  mw_file_destroy(file);
}

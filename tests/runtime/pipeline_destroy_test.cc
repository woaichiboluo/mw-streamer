#include "mw/streamer/api.h"

#include <chrono>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class TestDirectory final {
 public:
  TestDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("mw-pipeline-destroy-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void Check(MwResult result, const char* operation) {
  if (result != kMwResultSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + mw_last_error());
  }
}

std::filesystem::path WriteConfig(const TestDirectory& directory,
                                  const std::string& mode) {
  const auto path = directory.path() / "pipeline.toml";
  std::ofstream config(path);
  if (mode == "srt") {
    config << "[input]\ntype = 'zlm'\nurl = 'srt://127.0.0.1:1?streamid=missing'\n";
  } else if (mode == "network") {
    config << "[input]\ntype = 'zlm'\nurl = 'rtsp://localhost:1/missing'\n";
  } else {
    config << "[input]\n"
            "type = 'file'\n"
            "path = '"
         << (std::filesystem::path(MW_RUNTIME_TEST_DATA_DIR) / "h264_aac.mp4")
                .generic_string()
         << "'\n";
  }
  config << "downstream = ['record']\n"
            "[[sinks]]\n"
            "id = 'record'\n"
            "type = 'remux'\n"
            "target = '"
         << (directory.path() / "output.mp4").generic_string() << "'\n";
  config.close();
  if (config.fail()) {
    throw std::runtime_error("failed to write pipeline config");
  }
  return path;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string mode = argc > 1 ? argv[1] : "idle";
    TestDirectory directory;
    const auto config_path = WriteConfig(directory, mode);
    const auto config_path_string = config_path.string();
    MwPipelineCreateInfo create_info{};
    create_info.toml_path = config_path_string.c_str();

    if (mode == "race") {
      std::atomic<bool> go{false};
      std::array<MwResult, 2> results{};
      std::thread creator([&] {
        while (!go.load()) std::this_thread::yield();
        MwPipeline* pipeline = nullptr;
        results[0] = mw_pipeline_create_from_toml(&create_info, &pipeline);
        mw_pipeline_destroy(pipeline);
      });
      std::thread closer([&] {
        while (!go.load()) std::this_thread::yield();
        results[1] = mw_streamer_shutdown();
      });
      go.store(true);
      creator.join();
      closer.join();
      for (auto result : results) {
        if (result != kMwResultSuccess && result != kMwResultInvalidState) {
          throw std::runtime_error("unexpected create/shutdown race result");
        }
      }
      Check(mw_streamer_shutdown(), "shutdown after creation race failed");
    }
    if (mode == "cold") {
      Check(mw_streamer_shutdown(), "cold shutdown failed");
    }
    if (mode == "failed_build") {
      MwFrameCustomSinkBinding wrong_binding{};
      wrong_binding.sink_id = "record";
      create_info.frame_custom_sinks = &wrong_binding;
      create_info.frame_custom_sink_count = 1;
      MwPipeline* failed = nullptr;
      const auto result = mw_pipeline_create_from_toml(&create_info, &failed);
      if (result != kMwResultConfigError || failed) {
        mw_pipeline_destroy(failed);
        throw std::runtime_error("expected builder validation failure");
      }
      create_info.frame_custom_sinks = nullptr;
      create_info.frame_custom_sink_count = 0;
    }
    const bool create_pipelines = mode != "cold" && mode != "race" &&
                                  mode != "failed_build";
    for (int iteration = 0; create_pipelines && iteration < 10; ++iteration) {
      MwPipeline* pipeline = nullptr;
      Check(mw_pipeline_create_from_toml(&create_info, &pipeline),
            "pipeline create failed");
      if (pipeline == nullptr) {
        throw std::runtime_error("pipeline create returned null");
      }
      const auto rejected_shutdown = mw_streamer_shutdown();
      if (rejected_shutdown != kMwResultInvalidState) {
        mw_pipeline_destroy(pipeline);
        throw std::runtime_error("shutdown accepted a live Pipeline");
      }
      if (mode == "start" || mode == "network" || mode == "srt") {
        Check(mw_pipeline_start(pipeline), "pipeline start failed");
        mw_pipeline_stop(pipeline);
      }
      std::cout << "destroy begin: " << iteration << std::endl;
      mw_pipeline_destroy(pipeline);
      std::cout << "destroy end: " << iteration << std::endl;
    }
    std::array<MwResult, 4> shutdown_results{};
    std::array<std::thread, 4> shutdown_threads;
    for (std::size_t i = 0; i < shutdown_threads.size(); ++i) {
      shutdown_threads[i] = std::thread([&, i] {
        shutdown_results[i] = mw_streamer_shutdown();
      });
    }
    for (auto& thread : shutdown_threads) thread.join();
    for (auto result : shutdown_results) {
      Check(result, "concurrent shutdown failed");
    }
    Check(mw_streamer_shutdown(), "repeated shutdown failed");
    MwPipeline* rejected = nullptr;
    const auto result = mw_pipeline_create_from_toml(&create_info, &rejected);
    if (result != kMwResultInvalidState || rejected != nullptr ||
        std::string(mw_last_error()).empty()) {
      mw_pipeline_destroy(rejected);
      throw std::runtime_error("Pipeline creation after shutdown was not rejected");
    }
    std::cout << "runtime shutdown complete" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

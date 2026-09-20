#include "mw/streamer/api.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using Pipeline = std::unique_ptr<MwPipeline, decltype(&mw_pipeline_destroy)>;

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + ": " + mw_last_error());
  }
}

class TestDirectory {
 public:
  TestDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("mw-runtime-exit-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

Pipeline Create(const TestDirectory& directory, int index, bool network_input) {
  const auto config_path = directory.path / (std::to_string(index) + ".toml");
  std::ofstream config(config_path);
  config << "[input]\n";
  if (network_input) {
    config << "type = 'zlm'\nurl = 'rtsp://127.0.0.1:1/live/test'\n";
  } else {
    config << "type = 'file'\npath = '"
           << (std::filesystem::path(MW_RUNTIME_TEST_DATA_DIR) / "h264_aac.mp4")
                  .generic_string()
           << "'\n";
  }
  config << "downstream = ['record']\n";
  if (network_input) {
    config << "[input.player]\nconnect_timeout_ms = 500\n"
              "[input.reconnect_policy]\nmax_retries = 0\n";
  }
  config << "[[sinks]]\nid = 'record'\ntype = 'remux'\ntarget = '"
         << (directory.path / (std::to_string(index) + ".mp4")).generic_string()
         << "'\n";
  config.close();
  Check(!config.fail(), "write config failed");
  const auto path = config_path.string();
  MwPipelineCreateInfo info{};
  info.toml_path = path.c_str();
  MwPipeline* pipeline = nullptr;
  Check(mw_pipeline_create_from_toml(&info, &pipeline) == kMwResultSuccess,
        "create failed");
  Check(pipeline != nullptr, "create returned null");
  return Pipeline(pipeline, &mw_pipeline_destroy);
}

void RunToEnd(MwPipeline* pipeline) {
  Check(mw_pipeline_start(pipeline) == kMwResultSuccess, "start failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    MwInputStatus input{};
    Check(mw_pipeline_get_input_status(pipeline, &input) == kMwResultSuccess,
          "get input status failed");
    Check(input.state != kMwInputFailed, "input failed");
    if (input.state == kMwInputEnded) break;
    Check(std::chrono::steady_clock::now() < deadline, "input did not finish");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  mw_pipeline_stop(pipeline);
  MwPipelineState state{};
  Check(mw_pipeline_get_state(pipeline, &state) == kMwResultSuccess,
        "get state failed");
  Check(state == kMwPipelineStopped, "pipeline did not stop cleanly");
}

void RunToNetworkFailure(MwPipeline* pipeline) {
  Check(mw_pipeline_start(pipeline) == kMwResultSuccess, "network start failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    MwInputStatus input{};
    Check(mw_pipeline_get_input_status(pipeline, &input) == kMwResultSuccess,
          "get network input status failed");
    if (input.state == kMwInputFailed) break;
    Check(std::chrono::steady_clock::now() < deadline,
          "network input did not report connection failure");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  mw_pipeline_stop(pipeline);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "expected scenario");
    TestDirectory directory;
    const std::string scenario = argv[1];
    if (scenario == "create") {
      // Reproduce the original DLL exit hang without connecting to a server.
      for (int index = 0; index < 10; ++index) {
        auto pipeline = Create(directory, index, index % 2 == 0);
        pipeline.reset();
      }
    } else if (scenario == "immediate-stop") {
      // Stop while file delivery or network connection setup can be in flight.
      for (int index = 0; index < 10; ++index) {
        auto pipeline = Create(directory, index, index % 2 == 0);
        Check(mw_pipeline_start(pipeline.get()) == kMwResultSuccess,
              "immediate start failed");
        mw_pipeline_stop(pipeline.get());
        mw_pipeline_stop(pipeline.get());
        MwPipelineState state{};
        Check(mw_pipeline_get_state(pipeline.get(), &state) == kMwResultSuccess,
              "get stopped state failed");
        Check(state == kMwPipelineStopped, "immediate stop did not finish");
        pipeline.reset();
      }
    } else if (scenario == "shared") {
      auto first = Create(directory, 0, false);
      auto second = Create(directory, 1, false);
      first.reset();
      RunToEnd(second.get());
      second.reset();
    } else if (scenario == "recreate") {
      for (int index = 0; index < 5; ++index) {
        auto pipeline = Create(directory, index, false);
        RunToEnd(pipeline.get());
        pipeline.reset();
      }
    } else if (scenario == "network-start") {
      for (int index = 0; index < 3; ++index) {
        auto pipeline = Create(directory, index, true);
        RunToNetworkFailure(pipeline.get());
        pipeline.reset();
      }
    } else {
      throw std::runtime_error("unknown scenario");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}

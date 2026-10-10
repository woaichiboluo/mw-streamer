#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "mw/streamer/input/ffmpeg_input.h"

TEST_CASE("FFmpeg input follows native invalid and unknown option behavior",
          "[input][ffmpeg][options]") {
  using namespace std::chrono_literals;
  using mw::streamer::InputState;
  mw::streamer::FfmpegInputConfig config;
  config.mode = mw::streamer::InputMode::kRemux;
  config.auto_reconnect = false;
  bool invalid_value = false;
  SECTION("invalid known option fails opening") {
    config.options["probesize"] = "invalid";
    invalid_value = true;
  }
  SECTION("unknown option does not prevent opening") {
    config.options["mw_unknown_input_option"] = "unused";
  }
  std::mutex mutex;
  std::condition_variable changed;
  InputState terminal = InputState::kIdle;
  int error = 0;
  std::string message;
  size_t retries = 0;
  std::atomic<size_t> ready{0};
  std::atomic<size_t> packets{0};
  mw::streamer::FfmpegInput input(config);
  input.SetOnReady([&](const auto&) { ++ready; });
  input.SetOnPacket([&](std::uint64_t, const auto&) { ++packets; });
  input.SetOnStateChanged(
      [&](InputState state, int code, std::string_view description) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (state == InputState::kWaitingRetry) ++retries;
          if (state == InputState::kFailed || state == InputState::kEnded) {
            terminal = state;
            error = code;
            message = description;
          }
        }
        changed.notify_all();
      });
  input.Start(std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/h264_aac.mp4");
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, 8s, [&] {
      return terminal == InputState::kFailed || terminal == InputState::kEnded;
    }));
    if (invalid_value) {
      CHECK(terminal == InputState::kFailed);
      CHECK(error < 0);
      CHECK_FALSE(message.empty());
      CHECK(ready.load() == 0);
      CHECK(packets.load() == 0);
    } else {
      CHECK(terminal == InputState::kEnded);
      CHECK(ready.load() == 1);
      CHECK(packets.load() > 0);
    }
    CHECK(retries == 0);
  }
  input.Stop();
}

#include "mw/init/init.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/log.h>
}

#include "Util/logger.h"

namespace {

using StreamerLog = mw::log::Module<mw::log::LogModule::Streamer>;

class TemporaryLogFile {
 public:
  TemporaryLogFile() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-streamer-init-test-" + std::to_string(suffix) + ".log");
  }

  ~TemporaryLogFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

  std::string read() const {
    std::ifstream stream(path_);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
  }

 private:
  std::filesystem::path path_;
};

mw::InitConfig fileInitConfig(const std::filesystem::path& path) {
  mw::InitConfig config;
  config.log.console.enabled = false;
  config.log.rotating_file.enabled = true;
  config.log.rotating_file.path = path.string();
  config.log.rotating_file.level = mw::log::LogLevel::Trace;
  config.log.rotating_file.max_file_size = 1024 * 1024;
  config.log.rotating_file.max_files = 1;
  return config;
}

}  // namespace

TEST_CASE("init has a one-time process lifecycle", "[init][logging][lifecycle]") {
  CHECK_FALSE(mw::initialized());
  CHECK(mw::log::detail::shouldLog(mw::log::LogModule::Streamer,
                                   mw::log::LogLevel::Info));
  CHECK_FALSE(mw::log::detail::shouldLog(mw::log::LogModule::Streamer,
                                         mw::log::LogLevel::Debug));
  StreamerLog::info("default logger is available");

  mw::InitConfig invalid_config;
  invalid_config.log.console.enabled = false;
  CHECK_THROWS_AS(mw::init(invalid_config), std::invalid_argument);
  CHECK_FALSE(mw::initialized());

  TemporaryLogFile file;
  auto config = fileInitConfig(file.path());
  config.log.modules.streamer = mw::log::LogLevel::Debug;
  config.log.modules.zlm = mw::log::LogLevel::Info;
  config.log.modules.ffmpeg = mw::log::LogLevel::Info;
  config.log.async.enabled = true;
  config.log.async.queue_size = 128;
  config.log.async.overflow = mw::log::OverflowPolicy::Block;

  mw::init(config);
  CHECK(mw::initialized());

  // The first successful configuration wins. Later calls do not validate or
  // replace it.
  CHECK_NOTHROW(mw::init(invalid_config));

  StreamerLog::info("configured async message");
  InfoL << "zlm init bridge message";
  av_log(nullptr, AV_LOG_INFO, "ffmpeg init bridge message\n");
  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    started = true;
    while (!stop.load()) {
      StreamerLog::debug("concurrent shutdown message");
      std::this_thread::yield();
    }
  });

  while (!started.load()) {
    std::this_thread::yield();
  }
  mw::shutdown();
  stop = true;
  writer.join();

  CHECK_FALSE(mw::initialized());
  CHECK_NOTHROW(mw::shutdown());
  CHECK_THROWS_AS(mw::init(config), std::logic_error);
  CHECK(mw::log::detail::shouldLog(mw::log::LogModule::Streamer,
                                   mw::log::LogLevel::Info));

  const auto content = file.read();
  CHECK(content.find("[streamer] configured async message") !=
        std::string::npos);
  CHECK(content.find("[ZLM] zlm init bridge message") != std::string::npos);
  CHECK(content.find("[FFMPEG] ffmpeg init bridge message") !=
        std::string::npos);
}

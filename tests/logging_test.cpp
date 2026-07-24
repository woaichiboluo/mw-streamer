#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "mw/init/init.hpp"

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
            ("mw-streamer-log-test-" + std::to_string(suffix) + ".log");
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

mw::log::LogConfig fileLogConfig(const std::filesystem::path& path) {
  mw::log::LogConfig config;
  config.console.enabled = false;
  config.rotating_file.enabled = true;
  config.rotating_file.path = path.string();
  config.rotating_file.level = mw::log::LogLevel::Trace;
  config.rotating_file.max_file_size = 1024 * 1024;
  config.rotating_file.max_files = 1;
  return config;
}

}  // namespace

TEST_CASE("module level filters before the shared logger", "[logging]") {
  TemporaryLogFile file;
  auto config = fileLogConfig(file.path());
  config.modules.streamer = mw::log::LogLevel::Warning;

  {
    mw::log::Logging logging(config);
    StreamerLog::info("hidden info message");
    StreamerLog::warning("visible warning {}", 42);
  }

  const auto content = file.read();
  CHECK(content.find("hidden info message") == std::string::npos);
  CHECK(content.find("[streamer] visible warning 42") != std::string::npos);
}

TEST_CASE("ZLM and FFmpeg logs use module prefixes", "[logging][bridge]") {
  TemporaryLogFile file;
  auto config = fileLogConfig(file.path());
  config.modules.zlm = mw::log::LogLevel::Info;
  config.modules.ffmpeg = mw::log::LogLevel::Info;

  {
    mw::log::Logging logging(config);
    InfoL << "zlm bridge message";
    av_log(nullptr, AV_LOG_INFO, "ffmpeg bridge message\n");
  }

  const auto content = file.read();
  CHECK(content.find("[ZLM] zlm bridge message") != std::string::npos);
  CHECK(content.find("[FFMPEG] ffmpeg bridge message") != std::string::npos);
}

TEST_CASE("async logging drains its shared queue on destruction",
          "[logging][async]") {
  TemporaryLogFile file;
  auto config = fileLogConfig(file.path());
  config.async.enabled = true;
  config.async.queue_size = 128;
  config.async.overflow = mw::log::OverflowPolicy::Block;

  {
    mw::log::Logging logging(config);
    for (int index = 0; index < 32; ++index) {
      StreamerLog::info("async message {}", index);
    }
  }

  const auto content = file.read();
  CHECK(content.find("[streamer] async message 0") != std::string::npos);
  CHECK(content.find("[streamer] async message 31") != std::string::npos);
}

TEST_CASE("Logging can be owned manually without init",
          "[logging][lifecycle]") {
  TemporaryLogFile file;
  const auto config = fileLogConfig(file.path());

  {
    mw::log::Logging logging(config);
    CHECK_FALSE(mw::initialized());
    StreamerLog::info("manually owned logging");
  }

  const auto content = file.read();
  CHECK(content.find("[streamer] manually owned logging") != std::string::npos);
  CHECK(mw::log::detail::shouldLog(mw::log::LogModule::Streamer,
                                   mw::log::LogLevel::Info));
}

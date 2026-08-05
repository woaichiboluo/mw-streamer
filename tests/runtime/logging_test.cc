#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "mw/init/init.h"

extern "C" {
#include <libavutil/log.h>
}

#include "Util/logger.h"

namespace {

using StreamerLog =
    mw::streamer::log::Module<mw::streamer::log::LogModule::kStreamer>;
using ProcessorLog =
    mw::streamer::log::Module<mw::streamer::log::LogModule::kProcessor>;

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

  std::string Read() const {
    std::ifstream stream(path_);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
  }

 private:
  std::filesystem::path path_;
};

mw::streamer::log::LogConfig MakeFileLogConfig(
    const std::filesystem::path& path) {
  mw::streamer::log::LogConfig config;
  config.console.enabled = false;
  config.rotating_file.enabled = true;
  config.rotating_file.path = path.string();
  config.rotating_file.level = mw::streamer::log::LogLevel::kTrace;
  config.rotating_file.max_file_size = 1024 * 1024;
  config.rotating_file.max_files = 1;
  return config;
}

}  // namespace

TEST_CASE("module level filters before the shared logger", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.modules.streamer = mw::streamer::log::LogLevel::kWarning;

  {
    mw::streamer::log::Logging logging(config);
    StreamerLog::Info("hidden info message");
    StreamerLog::Warning("visible warning {}", 42);
  }

  const auto content = file.Read();
  CHECK(content.find("hidden info message") == std::string::npos);
  CHECK(content.find("[streamer] visible warning 42") != std::string::npos);
}

TEST_CASE("Processor logs use an independent module level", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.modules.streamer = mw::streamer::log::LogLevel::kOff;
  config.modules.processor = mw::streamer::log::LogLevel::kWarning;

  {
    mw::streamer::log::Logging logging(config);
    StreamerLog::Warning("hidden streamer warning");
    ProcessorLog::Info("hidden processor info");
    ProcessorLog::Warning("visible processor warning");
  }

  const auto content = file.Read();
  CHECK(content.find("hidden streamer warning") == std::string::npos);
  CHECK(content.find("hidden processor info") == std::string::npos);
  CHECK(content.find("[processor] visible processor warning") !=
        std::string::npos);
}

TEST_CASE("ZLM and FFmpeg logs use module prefixes", "[logging][bridge]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.modules.zlm = mw::streamer::log::LogLevel::kInfo;
  config.modules.ffmpeg = mw::streamer::log::LogLevel::kInfo;

  {
    mw::streamer::log::Logging logging(config);
    InfoL << "zlm bridge message";
    av_log(nullptr, AV_LOG_INFO, "ffmpeg bridge message\n");
  }

  const auto content = file.Read();
  CHECK(content.find("[ZLM] zlm bridge message") != std::string::npos);
  CHECK(content.find("[FFMPEG] ffmpeg bridge message") != std::string::npos);
}

TEST_CASE("async logging drains its shared queue on destruction",
          "[logging][async]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.async.enabled = true;
  config.async.queue_size = 128;
  config.async.overflow = mw::streamer::log::OverflowPolicy::kBlock;

  {
    mw::streamer::log::Logging logging(config);
    for (int index = 0; index < 32; ++index) {
      StreamerLog::Info("async message {}", index);
    }
  }

  const auto content = file.Read();
  CHECK(content.find("[streamer] async message 0") != std::string::npos);
  CHECK(content.find("[streamer] async message 31") != std::string::npos);
}

TEST_CASE("Logging can be owned manually without init",
          "[logging][lifecycle]") {
  TemporaryLogFile file;
  const auto config = MakeFileLogConfig(file.path());

  {
    mw::streamer::log::Logging logging(config);
    CHECK_FALSE(mw::streamer::IsInitialized());
    StreamerLog::Info("manually owned logging");
  }

  const auto content = file.Read();
  CHECK(content.find("[streamer] manually owned logging") != std::string::npos);
  CHECK(mw::streamer::log::detail::ShouldLog(
      mw::streamer::log::LogModule::kStreamer,
      mw::streamer::log::LogLevel::kInfo));
}

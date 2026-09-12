#include "mw/log.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>


extern "C" {
#include <libavutil/log.h>
}

#include "Util/logger.h"
#include "mw/streamer/log/internal/third_party_log_bridge.h"

namespace {

class TemporaryLogFile {
 public:
  TemporaryLogFile() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mw-log-test-" + std::to_string(suffix) + ".log");
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

mw::log::LogConfig MakeFileLogConfig(const std::filesystem::path& path) {
  mw::log::LogConfig config;
  config.console.level = mw::log::LogLevel::kOff;
  config.rotating_file.path = path.string();
  config.rotating_file.level = mw::log::LogLevel::kTrace;
  config.rotating_file.max_file_size = 1024 * 1024;
  config.rotating_file.max_files = 1;
  return config;
}

}  // namespace

TEST_CASE("named module level filters before fmt formatting", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.modules.push_back({"streamer", mw::log::LogLevel::kWarning});

  {
    mw::log::Logging logging(config);
    MW_LOG_INFO("streamer", "hidden info message");
    MW_LOG_WARNING("streamer", "visible warning {}", 42);
  }

  const auto content = file.Read();
  CHECK(content.find("hidden info message") == std::string::npos);
  CHECK(content.find("[streamer] visible warning 42") != std::string::npos);
  CHECK(content.find("[logging_test.cc:") != std::string::npos);
}

TEST_CASE("default macros use the default module", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());

  {
    mw::log::Logging logging(config);
    MW_LOG_INFO_DEFAULT("default message {}", 7);
  }

  CHECK(file.Read().find("[default] default message 7") != std::string::npos);
}

TEST_CASE("arbitrary modules use independent levels", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.level = mw::log::LogLevel::kOff;
  config.modules.push_back({"processor", mw::log::LogLevel::kWarning});

  {
    mw::log::Logging logging(config);
    MW_LOG_ERROR("streamer", "hidden streamer error");
    MW_LOG_INFO("processor", "hidden processor info");
    MW_LOG_WARNING("processor", "visible processor warning");
  }

  const auto content = file.Read();
  CHECK(content.find("hidden streamer error") == std::string::npos);
  CHECK(content.find("hidden processor info") == std::string::npos);
  CHECK(content.find("[processor] visible processor warning") !=
        std::string::npos);
}

TEST_CASE("third-party bridges preserve their module names",
          "[logging][bridge]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.modules.push_back({"zlm", mw::log::LogLevel::kInfo});
  config.modules.push_back({"ffmpeg", mw::log::LogLevel::kInfo});

  {
    mw::log::Logging logging(config);
    mw::streamer::internal::ThirdPartyLogBridge bridge;
    InfoL << "zlm bridge message";
    av_log(nullptr, AV_LOG_INFO, "ffmpeg bridge message\n");
  }

  const auto content = file.Read();
  CHECK(content.find("[zlm] zlm bridge message") != std::string::npos);
  CHECK(content.find("[ffmpeg] ffmpeg bridge message") != std::string::npos);
}

TEST_CASE("async logging drains on destruction", "[logging][async]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.async.enabled = true;
  config.async.queue_size = 128;
  config.async.overflow = mw::log::OverflowPolicy::kBlock;

  {
    mw::log::Logging logging(config);
    for (int index = 0; index < 32; ++index) {
      MW_LOG_INFO("streamer", "async message {}", index);
    }
  }

  const auto content = file.Read();
  CHECK(content.find("async message 0") != std::string::npos);
  CHECK(content.find("async message 31") != std::string::npos);
}

TEST_CASE("C ABI writes text with source location", "[logging][c-api]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());

  {
    mw::log::Logging logging(config);
    constexpr char kModule[] = "c-client";
    constexpr char kMessage[] = "plain C text";
    mw_log_write(kMwLogLevelInfo, kModule, sizeof(kModule) - 1, "client.c", 23,
                 kMessage, sizeof(kMessage) - 1);
  }

  const auto content = file.Read();
  CHECK(content.find("[c-client] plain C text") != std::string::npos);
  CHECK(content.find("[client.c:23]") != std::string::npos);
}

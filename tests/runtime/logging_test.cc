#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mw/log.h"

extern "C" {
#include <libavutil/log.h>
}

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

class TestLogConfig final {
 public:
  explicit TestLogConfig(const std::filesystem::path& path)
      : file_path_(path.string()) {
    mw_log_default_config(&config_);
    modules_.assign(config_.modules, config_.modules_size);
    config_.console_enabled = 0;
    config_.rotating_file_enabled = 1;
    config_.rotating_file_max_size = 1024 * 1024;
    config_.rotating_file_max_files = 1;
  }

  void SetModules(std::string modules) { modules_ = std::move(modules); }

  void EnableAsync(std::size_t queue_size,
                   MwLogOverflowPolicy overflow) noexcept {
    config_.async_enabled = 1;
    config_.async_queue_size = queue_size;
    config_.async_overflow = overflow;
  }

  const MwLogConfig& view() noexcept {
    config_.modules = modules_.data();
    config_.modules_size = modules_.size();
    config_.rotating_file_path = file_path_.data();
    config_.rotating_file_path_size = file_path_.size();
    return config_;
  }

 private:
  MwLogConfig config_{};
  std::string modules_;
  std::string file_path_;
};

TestLogConfig MakeFileLogConfig(const std::filesystem::path& path) {
  return TestLogConfig(path);
}

}  // namespace

TEST_CASE("default log config enables standard modules", "[logging]") {
  MwLogConfig config{};
  mw_log_default_config(&config);
  CHECK(std::string_view(config.modules, config.modules_size) ==
        "streamer;processor;");
  CHECK(config.console_enabled == 1);
  CHECK(config.rotating_file_enabled == 0);
}

TEST_CASE("module string accepts every level and trims whitespace",
          "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules(
      " trace-module : trace ;debug-module:debug;info-module:info;"
      "warn-module:warn;error-module:error;critical-module:critical;"
      "off-module:off;bare-module;;");

  mw::log::Logging logging(config.view());
  CHECK(mw::log::ShouldLog("trace-module", mw::log::LogLevel::kTrace));
  CHECK_FALSE(mw::log::ShouldLog("debug-module", mw::log::LogLevel::kTrace));
  CHECK(mw::log::ShouldLog("debug-module", mw::log::LogLevel::kDebug));
  CHECK(mw::log::ShouldLog("info-module", mw::log::LogLevel::kInfo));
  CHECK_FALSE(mw::log::ShouldLog("warn-module", mw::log::LogLevel::kInfo));
  CHECK(mw::log::ShouldLog("warn-module", mw::log::LogLevel::kWarning));
  CHECK(mw::log::ShouldLog("error-module", mw::log::LogLevel::kError));
  CHECK(mw::log::ShouldLog("critical-module", mw::log::LogLevel::kCritical));
  CHECK_FALSE(mw::log::ShouldLog("off-module", mw::log::LogLevel::kCritical));
  CHECK_FALSE(mw::log::ShouldLog("bare-module", mw::log::LogLevel::kDebug));
  CHECK(mw::log::ShouldLog("bare-module", mw::log::LogLevel::kInfo));
  CHECK_FALSE(mw::log::ShouldLog("unconfigured", mw::log::LogLevel::kWarning));
  CHECK(mw::log::ShouldLog("unconfigured", mw::log::LogLevel::kError));
  CHECK_FALSE(mw::log::ShouldLog("default", mw::log::LogLevel::kDebug));
  CHECK(mw::log::ShouldLog("default", mw::log::LogLevel::kInfo));
}

TEST_CASE("module string rejects invalid entries", "[logging]") {
  const auto invalid = [](std::string modules) {
    MwLogConfig config{};
    mw_log_default_config(&config);
    config.modules = modules.data();
    config.modules_size = modules.size();
    config.console_enabled = 0;
    CHECK_THROWS_AS(mw::log::Logging(config), std::invalid_argument);
  };

  invalid("streamer:verbose");
  invalid("streamer:info:debug");
  invalid("streamer; streamer:debug");
  invalid("   ;");
  invalid(":info");
}

TEST_CASE("Logging copies borrowed config strings", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("copied:warn;");
  mw::log::Logging logging(config.view());
  config.SetModules("copied:off;");
  CHECK(mw::log::ShouldLog("copied", mw::log::LogLevel::kWarning));
}

TEST_CASE("named module level filters before fmt formatting", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("streamer:warn;");

  {
    mw::log::Logging logging(config.view());
    MW_LOG_INFO("streamer", "hidden info message");
    MW_LOG_WARNING("streamer", "visible warning {}", 42);
  }

  const auto content = file.Read();
  CHECK(content.find("hidden info message") == std::string::npos);
  CHECK(content.find("[streamer] visible warning 42") != std::string::npos);
  CHECK(content.find("[logging_test.cc:") != std::string::npos);
}

TEST_CASE("unconfigured named modules default to error", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());

  {
    mw::log::Logging logging(config.view());
    MW_LOG_WARNING("other", "hidden other warning");
    MW_LOG_ERROR("other", "visible other error");
    MW_LOG_CRITICAL("other", "visible other critical");
  }

  const auto content = file.Read();
  CHECK(content.find("hidden other warning") == std::string::npos);
  CHECK(content.find("[other] visible other error") != std::string::npos);
  CHECK(content.find("[other] visible other critical") != std::string::npos);
}

TEST_CASE("default macros use the default module", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());

  {
    mw::log::Logging logging(config.view());
    MW_LOG_DEBUG_DEFAULT("hidden default debug");
    MW_LOG_INFO_DEFAULT("default message {}", 7);
  }

  const auto content = file.Read();
  CHECK(content.find("hidden default debug") == std::string::npos);
  CHECK(content.find("[default] default message 7") != std::string::npos);
}

TEST_CASE("explicit module levels override built-in levels", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("default:off;streamer:off;processor:warn;");

  {
    mw::log::Logging logging(config.view());
    MW_LOG_CRITICAL_DEFAULT("hidden default critical");
    MW_LOG_CRITICAL("streamer", "hidden streamer critical");
    MW_LOG_INFO("processor", "hidden processor info");
    MW_LOG_WARNING("processor", "visible processor warning");
  }

  const auto content = file.Read();
  CHECK(content.find("hidden default critical") == std::string::npos);
  CHECK(content.find("hidden streamer critical") == std::string::npos);
  CHECK(content.find("hidden processor info") == std::string::npos);
  CHECK(content.find("[processor] visible processor warning") !=
        std::string::npos);
}

TEST_CASE("direct ZLM logging and third-party bridges preserve module names",
          "[logging][bridge]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("zlm;ffmpeg;");

  {
    mw::log::Logging logging(config.view());
    mw::streamer::internal::ThirdPartyLogBridge bridge;
    MW_LOG_INFO("zlm", "zlm direct message");
    av_log(nullptr, AV_LOG_INFO, "ffmpeg bridge message\n");
  }

  const auto content = file.Read();
  CHECK(content.find("[zlm] zlm direct message") != std::string::npos);
  CHECK(content.find("[ffmpeg] ffmpeg bridge message") != std::string::npos);
}

TEST_CASE("unconfigured third-party modules default to error",
          "[logging][bridge]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());

  {
    mw::log::Logging logging(config.view());
    mw::streamer::internal::ThirdPartyLogBridge bridge;
    MW_LOG_INFO("zlm", "hidden zlm info");
    MW_LOG_ERROR("zlm", "visible zlm error");
    av_log(nullptr, AV_LOG_INFO, "hidden ffmpeg info\n");
    av_log(nullptr, AV_LOG_ERROR, "visible ffmpeg error\n");
  }

  const auto content = file.Read();
  CHECK(content.find("hidden zlm info") == std::string::npos);
  CHECK(content.find("[zlm] visible zlm error") != std::string::npos);
  CHECK(content.find("hidden ffmpeg info") == std::string::npos);
  CHECK(content.find("[ffmpeg] visible ffmpeg error") != std::string::npos);
}

TEST_CASE("direct ZLM logs preserve source locations and message boundaries",
          "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("zlm:trace;");
  {
    mw::log::Logging logging(config.view());
    mw::log::Write("zlm", mw::log::LogLevel::kWarning, "zlm_source.cc", 42,
                   "source preserved");
    for (int index = 0; index < 3; ++index) {
      MW_LOG_INFO("zlm", "unmerged message");
    }
    MW_LOG_TRACE("zlm", "zlm trace");
    MW_LOG_DEBUG("zlm", "zlm debug");
    MW_LOG_ERROR("zlm", "zlm error");
  }
  const auto content = file.Read();
  CHECK(content.find("[zlm_source.cc:42]") != std::string::npos);
  CHECK(content.find("[zlm] source preserved") != std::string::npos);
  CHECK(content.find("[zlm] zlm trace") != std::string::npos);
  CHECK(content.find("[zlm] zlm debug") != std::string::npos);
  CHECK(content.find("[zlm] zlm error") != std::string::npos);
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = content.find("[zlm] unmerged message", position)) !=
         std::string::npos) {
    ++count;
    ++position;
  }
  CHECK(count == 3);
}

TEST_CASE("concurrent ZLM logs go directly to the mw backend", "[logging]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("zlm;");
  constexpr int kThreads = 4;
  constexpr int kMessages = 200;
  {
    mw::log::Logging logging(config.view());
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
      threads.emplace_back([kMessages] {
        for (int message = 0; message < kMessages; ++message) {
          MW_LOG_INFO("zlm", "concurrent zlm message");
        }
      });
    }
    for (auto& thread : threads) thread.join();
  }
  const auto content = file.Read();
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = content.find("[zlm] concurrent zlm message", position)) !=
         std::string::npos) {
    ++count;
    ++position;
  }
  CHECK(count == kThreads * kMessages);
}

TEST_CASE("async logging drains on destruction", "[logging][async]") {
  TemporaryLogFile file;
  auto config = MakeFileLogConfig(file.path());
  config.SetModules("streamer;");
  config.EnableAsync(128, kMwLogOverflowBlock);

  {
    mw::log::Logging logging(config.view());
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
  config.SetModules("c-client;");

  {
    mw::log::Logging logging(config.view());
    constexpr char kModule[] = "c-client";
    constexpr char kMessage[] = "plain C text";
    mw_log_write(kMwLogLevelInfo, kModule, sizeof(kModule) - 1, "client.c", 23,
                 kMessage, sizeof(kMessage) - 1);
  }

  const auto content = file.Read();
  CHECK(content.find("[c-client] plain C text") != std::string::npos);
  CHECK(content.find("[client.c:23]") != std::string::npos);
}

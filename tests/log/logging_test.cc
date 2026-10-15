#include <fmt/format.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#include "mw/log.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using mw::log::LogConfig;
using mw::log::Logging;
using mw::log::LogLevel;
using mw::log::OverflowPolicy;

int FileDescriptor(std::FILE* file) {
#ifdef _WIN32
  return _fileno(file);
#else
  return fileno(file);
#endif
}

int DuplicateDescriptor(int descriptor) {
#ifdef _WIN32
  return _dup(descriptor);
#else
  return dup(descriptor);
#endif
}

bool RedirectDescriptor(int source, int destination) {
#ifdef _WIN32
  return _dup2(source, destination) == 0;
#else
  return dup2(source, destination) >= 0;
#endif
}

void CloseDescriptor(int descriptor) {
#ifdef _WIN32
  _close(descriptor);
#else
  close(descriptor);
#endif
}

class StdoutCapture {
 public:
  StdoutCapture() {
#ifdef _WIN32
    if (tmpfile_s(&file_) != 0) file_ = nullptr;
#else
    file_ = std::tmpfile();
#endif
    if (!file_) {
      throw std::runtime_error("cannot create stdout capture file");
    }
    std::fflush(stdout);
    saved_descriptor_ = DuplicateDescriptor(FileDescriptor(stdout));
    if (saved_descriptor_ < 0 ||
        !RedirectDescriptor(FileDescriptor(file_), FileDescriptor(stdout))) {
      if (saved_descriptor_ >= 0) CloseDescriptor(saved_descriptor_);
      std::fclose(file_);
      throw std::runtime_error("cannot redirect stdout");
    }
#ifdef _WIN32
    if (!SetStdHandle(
            STD_OUTPUT_HANDLE,
            reinterpret_cast<HANDLE>(_get_osfhandle(FileDescriptor(file_))))) {
      Restore();
      std::fclose(file_);
      throw std::runtime_error("cannot redirect Windows stdout handle");
    }
#endif
  }

  ~StdoutCapture() {
    Restore();
    std::fclose(file_);
  }

  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  std::string Finish() {
    Restore();
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
      throw std::runtime_error("cannot rewind stdout capture");
    }
    std::string output;
    char buffer[4096];
    while (const auto size = std::fread(buffer, 1, sizeof(buffer), file_)) {
      output.append(buffer, size);
    }
    if (std::ferror(file_)) {
      throw std::runtime_error("cannot read stdout capture");
    }
    return output;
  }

 private:
  void Restore() noexcept {
    if (saved_descriptor_ < 0) return;
    std::fflush(stdout);
    RedirectDescriptor(saved_descriptor_, FileDescriptor(stdout));
    CloseDescriptor(saved_descriptor_);
    saved_descriptor_ = -1;
#ifdef _WIN32
    SetStdHandle(
        STD_OUTPUT_HANDLE,
        reinterpret_cast<HANDLE>(_get_osfhandle(FileDescriptor(stdout))));
#endif
  }

  std::FILE* file_ = nullptr;
  int saved_descriptor_ = -1;
};

LogConfig ConsoleConfig() {
  LogConfig config;
  config.modules = {{"configured.module", LogLevel::kDebug},
                    {"muted.module", LogLevel::kOff}};
  config.console.color = false;
  return config;
}

}  // namespace

TEST_CASE(
    "default logging remains available across explicit logger lifetimes") {
  // Windows' default spdlog sink retains its initial stdout HANDLE. Capture
  // once before the first log call so the real sink works on both platforms.
  StdoutCapture capture;
  const bool default_info =
      mw::log::ShouldLog("unconfigured.module", mw::log::LogLevel::kInfo);
  const bool default_debug =
      mw::log::ShouldLog("unconfigured.module", mw::log::LogLevel::kDebug);
  const bool default_off =
      mw::log::ShouldLog("unconfigured.module", mw::log::LogLevel::kOff);
  MW_LOG_INFO("unconfigured.module", "default-info-{}", 42);
  MW_LOG_WARNING("unconfigured.module", "default-warning-probe");
  MW_LOG_ERROR("unconfigured.module", "default-error-probe");
  MW_LOG_DEBUG("unconfigured.module", "default-debug-hidden");
  mw::log::Write("unconfigured.module", mw::log::LogLevel::kOff, "off_probe.cc",
                 12, "default-off-hidden");
  mw::log::Write("direct.module", mw::log::LogLevel::kInfo,
                 "/path/direct_probe.cc", 123, "direct-write-probe");

  {
    auto config = ConsoleConfig();
    config.modules["业务支付:a;b"] = LogLevel::kDebug;
    mw::log::Logging logging(config);
    MW_LOG_DEBUG("configured.module", "configured-debug-probe");
    MW_LOG_INFO("unconfigured.module", "configured-info-hidden");
    MW_LOG_ERROR("muted.module", "configured-off-hidden");
    MW_LOG_DEBUG("业务支付:a;b", "custom-module-probe-{}", 42);
  }
  MW_LOG_INFO("unconfigured.module", "after-shutdown-probe");

  {
    auto config = ConsoleConfig();
    config.console.enabled = false;
    config.rotating_file.enabled = false;
    mw::log::Logging logging(config);
    MW_LOG_ERROR("configured.module", "disabled-sinks-hidden");
  }

  bool cpp_initialization_failed = false;
  try {
    auto config = ConsoleConfig();
    config.rotating_file.enabled = true;
    mw::log::Logging logging(config);
  } catch (const std::invalid_argument&) {
    cpp_initialization_failed = true;
  }
  MW_LOG_ERROR("unconfigured.module", "after-cpp-failure-probe");

  {
    auto config = ConsoleConfig();
    mw::log::Logging logging(config);
    MW_LOG_DEBUG("configured.module", "after-failure-configured-probe");
  }
  const auto output = capture.Finish();

  CHECK(default_info);
  CHECK_FALSE(default_debug);
  CHECK_FALSE(default_off);
  CHECK(cpp_initialization_failed);

  CHECK(output.find("[unconfigured.module] default-info-42") !=
        std::string::npos);
  for (const auto* marker :
       {"default-warning-probe", "default-error-probe",
        "configured-debug-probe", "after-shutdown-probe",
        "after-cpp-failure-probe", "after-failure-configured-probe"}) {
    CHECK(output.find(marker) != std::string::npos);
  }
  for (const auto* marker :
       {"default-debug-hidden", "default-off-hidden", "configured-info-hidden",
        "configured-off-hidden", "disabled-sinks-hidden"}) {
    CHECK(output.find(marker) == std::string::npos);
  }
  CHECK(
      output.find("[direct.module] direct-write-probe [direct_probe.cc:123]") !=
      std::string::npos);
  CHECK(output.find("[logging_test.cc:") != std::string::npos);
  CHECK(output.find("[业务支付:a;b] custom-module-probe-42") !=
        std::string::npos);
}

TEST_CASE("module levels preserve defaults and support complete replacement") {
  LogConfig config;
  config.console.enabled = false;
  {
    Logging logging(config);
    CHECK(mw::log::ShouldLog("streamer", LogLevel::kInfo));
    CHECK_FALSE(mw::log::ShouldLog("streamer", LogLevel::kDebug));
    for (const auto* module :
         {"perf.input", "perf.decoder.video", "perf.decoder.audio",
          "perf.encoder.video", "perf.encoder.audio",
          "perf.filter.encoder.audio"}) {
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kTrace));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kDebug));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kInfo));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kWarning));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kError));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kCritical));
    }
    CHECK(mw::log::ShouldLog("default", LogLevel::kInfo));
    CHECK(mw::log::ShouldLog("", LogLevel::kInfo));
    for (const auto* module : {"processor", "unknown.module"}) {
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kTrace));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kDebug));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kInfo));
      CHECK_FALSE(mw::log::ShouldLog(module, LogLevel::kWarning));
      CHECK(mw::log::ShouldLog(module, LogLevel::kError));
      CHECK(mw::log::ShouldLog(module, LogLevel::kCritical));
    }
  }
  config.modules["perf.input"] = LogLevel::kInfo;
  {
    Logging logging(config);
    CHECK(mw::log::ShouldLog("perf.input", LogLevel::kInfo));
    CHECK_FALSE(mw::log::ShouldLog("perf.input", LogLevel::kDebug));
    CHECK_FALSE(mw::log::ShouldLog("perf.encoder.video", LogLevel::kCritical));
  }
  config.modules.clear();
  {
    Logging logging(config);
    CHECK_FALSE(mw::log::ShouldLog("streamer", LogLevel::kInfo));
    CHECK_FALSE(mw::log::ShouldLog("perf.input", LogLevel::kInfo));
    CHECK(mw::log::ShouldLog("perf.input", LogLevel::kError));
    CHECK(mw::log::ShouldLog("default", LogLevel::kInfo));
  }
  config.modules = {{"default", LogLevel::kOff}};
  Logging logging(config);
  CHECK_FALSE(mw::log::ShouldLog("default", LogLevel::kCritical));
  CHECK_FALSE(mw::log::ShouldLog("", LogLevel::kCritical));
}

TEST_CASE("module names are owned and matched exactly without delimiters") {
  LogConfig config;
  config.console.enabled = false;
  std::string module = "业务支付:a;b";
  std::string path = "owned.log";
  config.modules = {{module, LogLevel::kDebug},
                    {"custom.parent", LogLevel::kTrace},
                    {"custom.info", LogLevel::kInfo}};
  config.rotating_file.path = path;
  module.assign("changed");
  path.clear();
  CHECK(config.rotating_file.path == "owned.log");

  Logging logging(config);
  config.modules.clear();
  CHECK(mw::log::ShouldLog("业务支付:a;b", LogLevel::kDebug));
  CHECK_FALSE(mw::log::ShouldLog("业务支付:a;b", LogLevel::kTrace));
  CHECK(mw::log::ShouldLog("custom.parent", LogLevel::kTrace));
  CHECK_FALSE(mw::log::ShouldLog("custom.parent.child", LogLevel::kDebug));
  CHECK_FALSE(mw::log::ShouldLog("业务支付", LogLevel::kDebug));
  CHECK_FALSE(mw::log::ShouldLog("changed", LogLevel::kDebug));
  CHECK(mw::log::ShouldLog("custom.info", LogLevel::kInfo));
  CHECK_FALSE(mw::log::ShouldLog("custom.info", LogLevel::kDebug));
}

TEST_CASE(
    "module configuration supports additions updates and removal by name") {
  LogConfig config;
  config.console.enabled = false;
  config.modules["custom.module"] = LogLevel::kTrace;
  config.modules["custom.module"] = LogLevel::kDebug;
  config.modules["perf.input"] = LogLevel::kTrace;
  {
    Logging logging(config);
    CHECK(mw::log::ShouldLog("streamer", LogLevel::kInfo));
    CHECK(mw::log::ShouldLog("custom.module", LogLevel::kDebug));
    CHECK_FALSE(mw::log::ShouldLog("custom.module", LogLevel::kTrace));
    CHECK(mw::log::ShouldLog("perf.input", LogLevel::kTrace));
    config.modules["custom.module"] = LogLevel::kOff;
    config.modules.erase("perf.input");
    CHECK(mw::log::ShouldLog("custom.module", LogLevel::kDebug));
    CHECK(mw::log::ShouldLog("perf.input", LogLevel::kTrace));
  }
  {
    Logging logging(config);
    CHECK(mw::log::ShouldLog("streamer", LogLevel::kInfo));
    CHECK_FALSE(mw::log::ShouldLog("custom.module", LogLevel::kCritical));
    CHECK_FALSE(mw::log::ShouldLog("perf.input", LogLevel::kInfo));
    config.modules.erase("custom.module");
    CHECK_FALSE(mw::log::ShouldLog("custom.module", LogLevel::kCritical));
  }
  Logging logging(config);
  CHECK(mw::log::ShouldLog("streamer", LogLevel::kInfo));
  CHECK(mw::log::ShouldLog("custom.module", LogLevel::kError));
  CHECK_FALSE(mw::log::ShouldLog("custom.module", LogLevel::kDebug));
}

TEST_CASE("invalid log configuration fails before publishing a logger") {
  LogConfig config;
  config.console.enabled = false;
  SECTION("empty module name") { config.modules[""] = LogLevel::kInfo; }
  SECTION("invalid module level") {
    config.modules["custom"] = static_cast<LogLevel>(255);
  }
  SECTION("invalid overflow policy") {
    config.async.overflow = static_cast<OverflowPolicy>(255);
  }
  CHECK_THROWS_AS(Logging{config}, std::invalid_argument);
  LogConfig valid_config;
  valid_config.console.enabled = false;
  CHECK_NOTHROW(Logging{valid_config});
}

TEST_CASE("async file logging drains pending messages on destruction") {
  struct TemporaryDirectory {
    TemporaryDirectory()
        : path(std::filesystem::temp_directory_path() /
               fmt::format("mw-log-async-{}", std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count())) {
      if (!std::filesystem::create_directory(path)) {
        throw std::runtime_error("cannot create temporary log directory");
      }
    }
    ~TemporaryDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
  } temporary;

  const auto log_path = temporary.path / "async.log";
  constexpr int kMessageCount = 128;
  {
    LogConfig config;
    config.console.enabled = false;
    config.rotating_file.enabled = true;
    config.rotating_file.path = log_path.string();
    config.async.enabled = true;
    config.async.queue_size = 4;
    config.async.overflow = OverflowPolicy::kBlock;
    config.modules = {{"async.module", LogLevel::kDebug},
                      {"muted.module", LogLevel::kOff}};
    Logging logging(config);
    for (int message = 0; message < kMessageCount; ++message) {
      MW_LOG_DEBUG("async.module", "async-message-{}|", message);
      MW_LOG_ERROR("muted.module", "muted-message-{}|", message);
    }
  }
  std::ifstream file(log_path);
  REQUIRE(file.is_open());
  const std::string output{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
  CHECK(std::count(output.begin(), output.end(), '\n') == kMessageCount);
  for (int message = 0; message < kMessageCount; ++message) {
    CHECK(output.find(fmt::format("[async.module] async-message-{}|",
                                  message)) != std::string::npos);
  }
  CHECK(output.find("muted-message-") == std::string::npos);
}

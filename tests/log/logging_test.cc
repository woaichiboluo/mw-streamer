#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

#include "mw/log.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" void WriteCLogProbe(void);

namespace {

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

MwLogConfig ConsoleConfig(std::string_view modules) {
  MwLogConfig config;
  mw_log_default_config(&config);
  config.modules = modules.data();
  config.modules_size = modules.size();
  config.console_color = 0;
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
  mw_log_write(kMwLogLevelOff, nullptr, 0, nullptr, 0, "c-off-hidden", 12);
  mw::log::Write("unconfigured.module", mw::log::LogLevel::kOff, "off_probe.cc",
                 12, "default-off-hidden");
  mw::log::Write("direct.module", mw::log::LogLevel::kInfo,
                 "/path/direct_probe.cc", 123, "direct-write-probe");
  WriteCLogProbe();

  {
    auto config = ConsoleConfig("configured.module:debug;muted.module:off");
    mw::log::Logging logging(config);
    MW_LOG_DEBUG("configured.module", "configured-debug-probe");
    MW_LOG_INFO("unconfigured.module", "configured-info-hidden");
    MW_LOG_ERROR("muted.module", "configured-off-hidden");
  }
  MW_LOG_INFO("unconfigured.module", "after-shutdown-probe");

  {
    auto config = ConsoleConfig("configured.module:debug");
    config.console_enabled = 0;
    config.rotating_file_enabled = 0;
    mw::log::Logging logging(config);
    MW_LOG_ERROR("configured.module", "disabled-sinks-hidden");
    WriteCLogProbe();
  }

  bool cpp_initialization_failed = false;
  try {
    auto config = ConsoleConfig("default");
    config.rotating_file_enabled = 1;
    mw::log::Logging logging(config);
  } catch (const std::invalid_argument&) {
    cpp_initialization_failed = true;
  }
  MW_LOG_ERROR("unconfigured.module", "after-cpp-failure-probe");

  auto invalid_config = ConsoleConfig("default");
  invalid_config.modules = nullptr;
  invalid_config.modules_size = 1;
  const auto failed_result = mw_log_initialize(&invalid_config);
  mw_log_write(kMwLogLevelError, "c.module", 8, "c_direct_probe.c", 456,
               "after-c-failure-probe", 21);

  const auto c_config = ConsoleConfig("c.module:debug");
  const auto initialized_result = mw_log_initialize(&c_config);
  WriteCLogProbe();
  mw_log_shutdown();
  const bool c_default_info = mw_log_should_log(kMwLogLevelInfo, "c.module", 8);
  const bool c_default_debug =
      mw_log_should_log(kMwLogLevelDebug, "c.module", 8);
  const bool c_default_off = mw_log_should_log(kMwLogLevelOff, "c.module", 8);
  mw_log_write(kMwLogLevelInfo, nullptr, 0, nullptr, 0,
               "c-after-shutdown-probe", 22);
  const auto output = capture.Finish();

  CHECK(default_info);
  CHECK_FALSE(default_debug);
  CHECK_FALSE(default_off);
  CHECK(c_default_info);
  CHECK_FALSE(c_default_debug);
  CHECK_FALSE(c_default_off);
  CHECK(cpp_initialization_failed);
  CHECK(failed_result == kMwLogInvalidArgument);
  CHECK(initialized_result == kMwLogSuccess);

  CHECK(output.find("[unconfigured.module] default-info-42") !=
        std::string::npos);
  for (const auto* marker :
       {"default-warning-probe", "default-error-probe", "c-info-probe",
        "c-error-probe", "configured-debug-probe", "after-shutdown-probe",
        "after-cpp-failure-probe", "after-c-failure-probe", "c-debug-probe"}) {
    CHECK(output.find(marker) != std::string::npos);
  }
  for (const auto* marker :
       {"default-debug-hidden", "default-off-hidden", "c-off-hidden",
        "configured-info-hidden", "configured-off-hidden",
        "disabled-sinks-hidden"}) {
    CHECK(output.find(marker) == std::string::npos);
  }
  for (const auto* marker :
       {"c-info-probe", "c-error-probe", "c-debug-probe"}) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = output.find(marker, position)) != std::string::npos) {
      ++count;
      ++position;
    }
    CHECK(count == (std::string_view(marker) == "c-debug-probe" ? 1U : 2U));
  }
  CHECK(
      output.find("[direct.module] direct-write-probe [direct_probe.cc:123]") !=
      std::string::npos);
  CHECK(output.find("[logging_test.cc:") != std::string::npos);
  CHECK(output.find("[logging_c_probe.c:") != std::string::npos);
  CHECK(output.find("[c_direct_probe.c:456]") != std::string::npos);
  CHECK(output.find("[default] c-after-shutdown-probe") != std::string::npos);
}

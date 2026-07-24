#pragma once

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mw::log {

enum class LogLevel : std::uint8_t {
  Off,
  Trace,
  Debug,
  Info,
  Warning,
  Error,
  Critical,
};

enum class LogModule : std::uint8_t {
  Zlm,
  Srt,
  Ffmpeg,
  Streamer,
  Count,
};

enum class OverflowPolicy : std::uint8_t {
  Block,
  OverrunOldest,
};

struct LogModuleConfig {
  LogLevel zlm = LogLevel::Off;
  LogLevel srt = LogLevel::Off;
  LogLevel ffmpeg = LogLevel::Off;
  LogLevel streamer = LogLevel::Info;
};

struct ConsoleSinkConfig {
  bool enabled = true;
  bool color = true;
  LogLevel level = LogLevel::Trace;
};

struct RotatingFileSinkConfig {
  bool enabled = false;
  std::string path;
  LogLevel level = LogLevel::Trace;
  std::size_t max_file_size = 10 * 1024 * 1024;
  std::size_t max_files = 5;
};

struct AsyncConfig {
  bool enabled = false;
  std::size_t queue_size = 8192;
  OverflowPolicy overflow = OverflowPolicy::OverrunOldest;
};

struct LogConfig {
  LogModuleConfig modules;
  ConsoleSinkConfig console;
  RotatingFileSinkConfig rotating_file;
  AsyncConfig async;
};

namespace detail {

class LoggingImpl;

bool shouldLog(LogModule module, LogLevel level) noexcept;
void write(LogModule module, LogLevel level, std::string_view message);

template <typename... Args>
void write(LogModule module, LogLevel level, fmt::format_string<Args...> format,
           Args&&... args) {
  if (!shouldLog(module, level)) {
    return;
  }
  write(module, level, fmt::format(format, std::forward<Args>(args)...));
}

}  // namespace detail

class Logging {
 public:
  explicit Logging(const LogConfig& config);
  ~Logging();

  Logging(const Logging&) = delete;
  Logging& operator=(const Logging&) = delete;
  Logging(Logging&&) = delete;
  Logging& operator=(Logging&&) = delete;

 private:
  std::unique_ptr<detail::LoggingImpl> impl_;
};

template <LogModule module>
struct Module {
  template <typename... Args>
  static void trace(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Trace, format, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void debug(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Debug, format, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void info(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Info, format, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void warning(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Warning, format,
                  std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void error(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Error, format, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void critical(fmt::format_string<Args...> format, Args&&... args) {
    detail::write(module, LogLevel::Critical, format,
                  std::forward<Args>(args)...);
  }
};

}  // namespace mw::log

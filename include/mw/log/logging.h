#ifndef MW_STREAMER_INCLUDE_MW_LOG_LOGGING_H_
#define MW_STREAMER_INCLUDE_MW_LOG_LOGGING_H_

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mw::streamer::log {

enum class LogLevel : std::uint8_t {
  kOff,
  kTrace,
  kDebug,
  kInfo,
  kWarning,
  kError,
  kCritical,
};

enum class LogModule : std::uint8_t {
  kZlm,
  kSrt,
  kFfmpeg,
  kStreamer,
  kProcessor,
  kCount,
};

enum class OverflowPolicy : std::uint8_t {
  kBlock,
  kOverrunOldest,
};

struct LogModuleConfig {
  LogLevel zlm = LogLevel::kOff;
  LogLevel srt = LogLevel::kOff;
  LogLevel ffmpeg = LogLevel::kOff;
  LogLevel streamer = LogLevel::kInfo;
  LogLevel processor = LogLevel::kInfo;
};

struct ConsoleSinkConfig {
  bool enabled = true;
  bool color = true;
  LogLevel level = LogLevel::kTrace;
};

struct RotatingFileSinkConfig {
  bool enabled = false;
  std::string path;
  LogLevel level = LogLevel::kTrace;
  std::size_t max_file_size = 10 * 1024 * 1024;
  std::size_t max_files = 5;
};

struct AsyncConfig {
  bool enabled = false;
  std::size_t queue_size = 8192;
  OverflowPolicy overflow = OverflowPolicy::kOverrunOldest;
};

struct LogConfig {
  LogModuleConfig modules;
  ConsoleSinkConfig console;
  RotatingFileSinkConfig rotating_file;
  AsyncConfig async;
};

namespace detail {

class LoggingImpl;

bool ShouldLog(LogModule module, LogLevel level) noexcept;
void Write(LogModule module, LogLevel level, std::string_view message);

template <typename... Args>
void Write(LogModule module, LogLevel level, fmt::format_string<Args...> format,
           Args&&... args) {
  if (!ShouldLog(module, level)) {
    return;
  }
  Write(module, level, fmt::format(format, std::forward<Args>(args)...));
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
  static void Trace(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kTrace, format,
                  std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void Debug(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kDebug, format,
                  std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void Info(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kInfo, format, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void Warning(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kWarning, format,
                  std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void Error(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kError, format,
                  std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void Critical(fmt::format_string<Args...> format, Args&&... args) {
    detail::Write(module, LogLevel::kCritical, format,
                  std::forward<Args>(args)...);
  }
};

}  // namespace mw::streamer::log

#endif  // MW_STREAMER_INCLUDE_MW_LOG_LOGGING_H_

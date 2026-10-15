#ifndef MW_LOG_H_
#define MW_LOG_H_

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "mw/export.h"

namespace mw::log {

enum class LogLevel : std::uint8_t {
  kTrace,
  kDebug,
  kInfo,
  kWarning,
  kError,
  kCritical,
  kOff,
};

enum class OverflowPolicy : std::uint8_t {
  kBlock,
  kOverrunOldest,
};

struct ConsoleConfig {
  bool enabled = true;
  bool color = true;
};

struct RotatingFileConfig {
  bool enabled = false;
  std::string path;
  // Size in bytes. Size and retained file count must be positive when enabled.
  std::size_t max_size = 10 * 1024 * 1024;
  std::size_t max_files = 5;
};

struct AsyncConfig {
  bool enabled = false;
  // Number of pending messages; must be positive for an active async logger.
  std::size_t queue_size = 8192;
  OverflowPolicy overflow = OverflowPolicy::kOverrunOldest;
};

struct LogConfig {
  // Arbitrary nonempty names are matched exactly; dots have no hierarchy.
  // Assign or erase individual keys to change their levels. Replacing this map
  // replaces all explicit levels. Unlisted modules use error, except "default",
  // which uses info. Logging copies the map; edits affect the next instance.
  std::map<std::string, LogLevel> modules = {
      {"streamer", LogLevel::kInfo},
      {"perf.input", LogLevel::kOff},
      {"perf.decoder.video", LogLevel::kOff},
      {"perf.decoder.audio", LogLevel::kOff},
      {"perf.encoder.video", LogLevel::kOff},
      {"perf.encoder.audio", LogLevel::kOff},
      {"perf.filter.input.video", LogLevel::kOff},
      {"perf.filter.input.audio", LogLevel::kOff},
      {"perf.filter.encoder.audio", LogLevel::kOff},
      {"perf.filter.video", LogLevel::kOff},
      {"perf.filter.audio", LogLevel::kOff},
      {"default", LogLevel::kInfo}};
  ConsoleConfig console;
  RotatingFileConfig rotating_file;
  AsyncConfig async;
};

class LoggingImpl;

// Logging calls are thread-safe while the active Logging object is alive.
// Before initialization and after shutdown, calls use spdlog's default logger
// and level. Explicit configuration takes precedence, including disabled sinks.
MW_LOG_API bool ShouldLog(std::string_view module, LogLevel level) noexcept;
MW_LOG_API void Write(std::string_view module, LogLevel level, const char* file,
                      std::uint32_t line, std::string_view message) noexcept;
MW_LOG_API void WriteFormattedArgs(std::string_view module, LogLevel level,
                                   const char* file, std::uint32_t line,
                                   fmt::string_view format,
                                   fmt::format_args args) noexcept;

template <typename... Args>
void WriteFormatted(std::string_view module, LogLevel level, const char* file,
                    std::uint32_t line, fmt::format_string<Args...> format,
                    Args&&... args) noexcept {
  WriteFormattedArgs(module, level, file, line, format.get(),
                     fmt::make_format_args(args...));
}

class MW_LOG_API Logging {
 public:
  // Owns a process-wide logger and copies the configuration. Only one instance
  // may be active. Invalid configuration throws std::invalid_argument; an
  // already active instance throws std::logic_error. Sink failures propagate.
  explicit Logging(const LogConfig& config = {});
  // Stop concurrent logging before destruction. Flushes and joins the owned
  // asynchronous worker before releasing its sinks.
  ~Logging();

  Logging(const Logging&) = delete;
  Logging& operator=(const Logging&) = delete;
  Logging(Logging&&) = delete;
  Logging& operator=(Logging&&) = delete;

 private:
  std::unique_ptr<LoggingImpl> impl_;
};

}  // namespace mw::log

#define MW_LOG_DETAIL(level, module, ...)                           \
  do {                                                              \
    const std::string_view mw_log_module_{module};                  \
    ::mw::log::WriteFormatted(mw_log_module_, level, __FILE__,      \
                              static_cast<std::uint32_t>(__LINE__), \
                              __VA_ARGS__);                         \
  } while (false)

#define MW_LOG_TRACE(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kTrace, module, __VA_ARGS__)
#define MW_LOG_DEBUG(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kDebug, module, __VA_ARGS__)
#define MW_LOG_INFO(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kInfo, module, __VA_ARGS__)
#define MW_LOG_WARNING(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kWarning, module, __VA_ARGS__)
#define MW_LOG_ERROR(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kError, module, __VA_ARGS__)
#define MW_LOG_CRITICAL(module, ...) \
  MW_LOG_DETAIL(::mw::log::LogLevel::kCritical, module, __VA_ARGS__)

#define MW_LOG_TRACE_DEFAULT(...) MW_LOG_TRACE("default", __VA_ARGS__)
#define MW_LOG_DEBUG_DEFAULT(...) MW_LOG_DEBUG("default", __VA_ARGS__)
#define MW_LOG_INFO_DEFAULT(...) MW_LOG_INFO("default", __VA_ARGS__)
#define MW_LOG_WARNING_DEFAULT(...) MW_LOG_WARNING("default", __VA_ARGS__)
#define MW_LOG_ERROR_DEFAULT(...) MW_LOG_ERROR("default", __VA_ARGS__)
#define MW_LOG_CRITICAL_DEFAULT(...) MW_LOG_CRITICAL("default", __VA_ARGS__)

#endif  // MW_LOG_H_

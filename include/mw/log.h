#ifndef MW_LOG_H_
#define MW_LOG_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MwLogLevel {
  kMwLogLevelTrace,
  kMwLogLevelDebug,
  kMwLogLevelInfo,
  kMwLogLevelWarning,
  kMwLogLevelError,
  kMwLogLevelCritical,
  kMwLogLevelOff,
} MwLogLevel;

typedef enum MwLogOverflowPolicy {
  kMwLogOverflowBlock,
  kMwLogOverflowOverrunOldest,
} MwLogOverflowPolicy;

typedef enum MwLogResult {
  kMwLogSuccess,
  kMwLogInvalidArgument,
  kMwLogAlreadyInitialized,
  kMwLogInternalError,
} MwLogResult;

typedef struct MwLogModuleConfig {
  const char* name;
  size_t name_size;
  MwLogLevel level;
} MwLogModuleConfig;

typedef struct MwLogConfig {
  uint32_t struct_size;
  MwLogLevel level;
  const MwLogModuleConfig* modules;
  size_t module_count;
  MwLogLevel console_level;
  int console_color;
  MwLogLevel file_level;
  const char* file_path;
  size_t file_path_size;
  size_t max_file_size;
  size_t max_files;
  int async_enabled;
  size_t async_queue_size;
  MwLogOverflowPolicy async_overflow;
} MwLogConfig;

MW_LOG_API void mw_log_default_config(MwLogConfig* config);
MW_LOG_API MwLogResult mw_log_initialize(const MwLogConfig* config);
MW_LOG_API void mw_log_shutdown(void);

MW_LOG_API int mw_log_should_log(MwLogLevel level, const char* module,
                                 size_t module_size);
MW_LOG_API void mw_log_write(MwLogLevel level, const char* module,
                             size_t module_size, const char* file,
                             uint32_t line, const char* message,
                             size_t message_size);

#ifdef __cplusplus
}

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

struct ModuleLogConfig {
  std::string name;
  LogLevel level = LogLevel::kInfo;
};

struct ConsoleSinkConfig {
  bool color = true;
  LogLevel level = LogLevel::kTrace;
};

struct RotatingFileSinkConfig {
  std::string path;
  LogLevel level = LogLevel::kOff;
  std::size_t max_file_size = 10 * 1024 * 1024;
  std::size_t max_files = 5;
};

struct AsyncConfig {
  bool enabled = false;
  std::size_t queue_size = 8192;
  OverflowPolicy overflow = OverflowPolicy::kOverrunOldest;
};

struct LogConfig {
  LogLevel level = LogLevel::kInfo;
  std::vector<ModuleLogConfig> modules;
  ConsoleSinkConfig console;
  RotatingFileSinkConfig rotating_file;
  AsyncConfig async;
};

class LoggingImpl;

MW_LOG_API bool ShouldLog(std::string_view module, LogLevel level) noexcept;
MW_LOG_API void Write(std::string_view module, LogLevel level, const char* file,
                      std::uint32_t line, std::string_view message);

template <typename... Args>
void WriteFormatted(std::string_view module, LogLevel level, const char* file,
                    std::uint32_t line, fmt::format_string<Args...> format,
                    Args&&... args) {
  Write(module, level, file, line,
        fmt::format(format, std::forward<Args>(args)...));
}

class MW_LOG_API Logging {
 public:
  explicit Logging(const LogConfig& config);
  ~Logging();

  Logging(const Logging&) = delete;
  Logging& operator=(const Logging&) = delete;
  Logging(Logging&&) = delete;
  Logging& operator=(Logging&&) = delete;

 private:
  std::unique_ptr<LoggingImpl> impl_;
};

}  // namespace mw::log

#define MW_LOG_DETAIL(level, module, ...)                             \
  do {                                                                \
    const std::string_view mw_log_module_{module};                    \
    if (::mw::log::ShouldLog(mw_log_module_, level)) {                \
      ::mw::log::WriteFormatted(mw_log_module_, level, __FILE__,      \
                                static_cast<std::uint32_t>(__LINE__), \
                                __VA_ARGS__);                         \
    }                                                                 \
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

#else

#include <string.h>

#define MW_LOG_C_WRITE(level, module, message)                           \
  do {                                                                   \
    const char* const mw_log_module_ = (module);                         \
    const size_t mw_log_module_size_ =                                   \
        mw_log_module_ ? strlen(mw_log_module_) : 0;                     \
    if (mw_log_should_log(level, mw_log_module_, mw_log_module_size_)) { \
      const char* const mw_log_message_ = (message);                     \
      mw_log_write(level, mw_log_module_, mw_log_module_size_, __FILE__, \
                   (uint32_t)__LINE__, mw_log_message_,                  \
                   mw_log_message_ ? strlen(mw_log_message_) : 0);       \
    }                                                                    \
  } while (0)

#define MW_LOG_TRACE(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelTrace, module, message)
#define MW_LOG_DEBUG(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelDebug, module, message)
#define MW_LOG_INFO(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelInfo, module, message)
#define MW_LOG_WARNING(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelWarning, module, message)
#define MW_LOG_ERROR(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelError, module, message)
#define MW_LOG_CRITICAL(module, message) \
  MW_LOG_C_WRITE(kMwLogLevelCritical, module, message)

#define MW_LOG_TRACE_DEFAULT(message) MW_LOG_TRACE("default", message)
#define MW_LOG_DEBUG_DEFAULT(message) MW_LOG_DEBUG("default", message)
#define MW_LOG_INFO_DEFAULT(message) MW_LOG_INFO("default", message)
#define MW_LOG_WARNING_DEFAULT(message) MW_LOG_WARNING("default", message)
#define MW_LOG_ERROR_DEFAULT(message) MW_LOG_ERROR("default", message)
#define MW_LOG_CRITICAL_DEFAULT(message) MW_LOG_CRITICAL("default", message)

#endif  // __cplusplus

#endif  // MW_LOG_H_

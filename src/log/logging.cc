#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mw/log.h"

namespace mw::log {
namespace {

constexpr std::string_view kDefaultModule = "default";
constexpr char kDefaultModules[] = "streamer;processor;";
constexpr std::string_view kLogPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";

std::string_view NormalizeModule(std::string_view module) noexcept {
  return module.empty() ? kDefaultModule : module;
}

LogLevel BuiltInModuleLevel(std::string_view module) noexcept {
  return module == kDefaultModule ? LogLevel::kInfo : LogLevel::kError;
}

std::string_view FileName(const char* file) noexcept {
  if (!file) return {};
  const std::string_view path(file);
  const auto separator = path.find_last_of("/\\");
  return separator == std::string_view::npos ? path
                                             : path.substr(separator + 1);
}

int LevelRank(LogLevel level) noexcept { return static_cast<int>(level); }

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return spdlog::level::trace;
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarning:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kCritical:
      return spdlog::level::critical;
    case LogLevel::kOff:
      return spdlog::level::off;
  }
  return spdlog::level::off;
}

bool FromCLevel(MwLogLevel input, LogLevel* output) noexcept {
  switch (input) {
    case kMwLogLevelTrace:
      *output = LogLevel::kTrace;
      return true;
    case kMwLogLevelDebug:
      *output = LogLevel::kDebug;
      return true;
    case kMwLogLevelInfo:
      *output = LogLevel::kInfo;
      return true;
    case kMwLogLevelWarning:
      *output = LogLevel::kWarning;
      return true;
    case kMwLogLevelError:
      *output = LogLevel::kError;
      return true;
    case kMwLogLevelCritical:
      *output = LogLevel::kCritical;
      return true;
    case kMwLogLevelOff:
      *output = LogLevel::kOff;
      return true;
  }
  return false;
}

bool ParseLevel(std::string_view input, LogLevel* output) noexcept {
  if (input == "trace") {
    *output = LogLevel::kTrace;
  } else if (input == "debug") {
    *output = LogLevel::kDebug;
  } else if (input == "info") {
    *output = LogLevel::kInfo;
  } else if (input == "warn") {
    *output = LogLevel::kWarning;
  } else if (input == "error") {
    *output = LogLevel::kError;
  } else if (input == "critical") {
    *output = LogLevel::kCritical;
  } else if (input == "off") {
    *output = LogLevel::kOff;
  } else {
    return false;
  }
  return true;
}

std::string_view Trim(std::string_view input) noexcept {
  constexpr std::string_view kWhitespace = " \t\r\n";
  const auto begin = input.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos) return {};
  const auto end = input.find_last_not_of(kWhitespace);
  return input.substr(begin, end - begin + 1);
}

}  // namespace

class LoggingImpl {
 public:
  explicit LoggingImpl(const MwLogConfig& config, bool publish = true) {
    Validate(config);
    ParseModules(config);
    CreateSinks(config);
    CreateLogger(config);
    if (publish) {
      Publish();
    }
  }

  ~LoggingImpl() { Shutdown(); }

  bool ShouldLog(std::string_view module, LogLevel level) const noexcept {
    if (level == LogLevel::kOff) {
      return false;
    }
    const auto normalized = NormalizeModule(module);
    const auto iterator = std::find_if(
        module_levels_.begin(), module_levels_.end(),
        [normalized](const auto& entry) { return entry.first == normalized; });
    const auto configured = iterator == module_levels_.end()
                                ? BuiltInModuleLevel(normalized)
                                : iterator->second;
    return configured != LogLevel::kOff &&
           LevelRank(level) >= LevelRank(configured);
  }

  void Write(std::string_view module, LogLevel level, const char* file,
             std::uint32_t line, std::string_view message) {
    if (!ShouldLog(module, level)) {
      return;
    }
    logger_->log(ToSpdlogLevel(level), "[{}] {} [{}:{}]",
                 NormalizeModule(module), message, FileName(file), line);
  }

  void WriteFormatted(std::string_view module, LogLevel level, const char* file,
                      std::uint32_t line, fmt::string_view format,
                      fmt::format_args args) {
    if (!ShouldLog(module, level)) {
      return;
    }
    logger_->log(ToSpdlogLevel(level), "[{}] {} [{}:{}]",
                 NormalizeModule(module), fmt::vformat(format, args),
                 FileName(file), line);
  }

 private:
  static void Validate(const MwLogConfig& config) {
    if (!config.modules && config.modules_size != 0) {
      throw std::invalid_argument("log modules cannot be null");
    }
    if (!config.rotating_file_path && config.rotating_file_path_size != 0) {
      throw std::invalid_argument("rotating log file path cannot be null");
    }
    if (config.async_overflow != kMwLogOverflowBlock &&
        config.async_overflow != kMwLogOverflowOverrunOldest) {
      throw std::invalid_argument("invalid log overflow policy");
    }
  }

  void ParseModules(const MwLogConfig& config) {
    const std::string_view modules =
        config.modules ? std::string_view(config.modules, config.modules_size)
                       : std::string_view{};
    std::size_t begin = 0;
    while (begin < modules.size()) {
      const auto separator = modules.find(';', begin);
      const auto end =
          separator == std::string_view::npos ? modules.size() : separator;
      const auto raw_entry = modules.substr(begin, end - begin);
      if (!raw_entry.empty()) {
        const auto entry = Trim(raw_entry);
        if (entry.empty()) {
          throw std::invalid_argument("log module name cannot be empty");
        }
        const auto colon = entry.find(':');
        const auto name = Trim(entry.substr(0, colon));
        if (name.empty() ||
            (colon != std::string_view::npos &&
             entry.find(':', colon + 1) != std::string_view::npos)) {
          throw std::invalid_argument("invalid log module entry");
        }
        LogLevel level = LogLevel::kInfo;
        if (colon != std::string_view::npos &&
            !ParseLevel(Trim(entry.substr(colon + 1)), &level)) {
          throw std::invalid_argument("invalid log module level");
        }
        const auto duplicate = std::find_if(
            module_levels_.begin(), module_levels_.end(),
            [name](const auto& item) { return item.first == name; });
        if (duplicate != module_levels_.end()) {
          throw std::invalid_argument("duplicate log module: " +
                                      std::string(name));
        }
        module_levels_.emplace_back(name, level);
      }
      if (separator == std::string_view::npos) break;
      begin = separator + 1;
    }
  }

  void CreateSinks(const MwLogConfig& config) {
    if (config.console_enabled) {
      spdlog::sink_ptr sink;
      if (config.console_color) {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      } else {
        sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      }
      sink->set_level(spdlog::level::trace);
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (config.rotating_file_enabled) {
      const std::string path = config.rotating_file_path
                                   ? std::string(config.rotating_file_path,
                                                 config.rotating_file_path_size)
                                   : std::string{};
      if (path.empty()) {
        throw std::invalid_argument("rotating log file path cannot be empty");
      }
      if (config.rotating_file_max_size == 0 ||
          config.rotating_file_max_files == 0) {
        throw std::invalid_argument(
            "rotating log file size and count must be greater than zero");
      }
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          path, config.rotating_file_max_size, config.rotating_file_max_files);
      sink->set_level(spdlog::level::trace);
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }
  }

  void CreateLogger(const MwLogConfig& config) {
    if (config.async_enabled && !sinks_.empty()) {
      if (config.async_queue_size == 0) {
        throw std::invalid_argument(
            "async log queue size must be greater than zero");
      }
      thread_pool_ = std::make_shared<spdlog::details::thread_pool>(
          config.async_queue_size, 1);
      const auto overflow = config.async_overflow == kMwLogOverflowBlock
                                ? spdlog::async_overflow_policy::block
                                : spdlog::async_overflow_policy::overrun_oldest;
      logger_ = std::make_shared<spdlog::async_logger>(
          "mw-log", sinks_.begin(), sinks_.end(), thread_pool_, overflow);
    } else {
      logger_ = std::make_shared<spdlog::logger>("mw-log", sinks_.begin(),
                                                 sinks_.end());
    }
    logger_->set_level(spdlog::level::trace);
  }

  void Publish();
  void Shutdown() noexcept;

  std::vector<std::pair<std::string, LogLevel>> module_levels_;
  std::vector<spdlog::sink_ptr> sinks_;
  std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
  std::shared_ptr<spdlog::logger> logger_;
  bool published_ = false;
};

std::atomic<LoggingImpl*> g_active_logging{nullptr};

void LoggingImpl::Publish() {
  LoggingImpl* expected = nullptr;
  if (!g_active_logging.compare_exchange_strong(expected, this,
                                                std::memory_order_release,
                                                std::memory_order_acquire)) {
    throw std::logic_error("mw log is already initialized");
  }
  published_ = true;
}

void LoggingImpl::Shutdown() noexcept {
  if (published_) {
    LoggingImpl* expected = this;
    g_active_logging.compare_exchange_strong(expected, nullptr,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    published_ = false;
  }
  if (logger_) {
    try {
      logger_->flush();
    } catch (...) {
    }
    logger_.reset();
  }
  thread_pool_.reset();
  sinks_.clear();
}

bool ShouldLog(std::string_view module, LogLevel level) noexcept {
  auto* logging = g_active_logging.load(std::memory_order_acquire);
  if (logging) {
    return logging->ShouldLog(module, level);
  }
  return false;
}

void Write(std::string_view module, LogLevel level, const char* file,
           std::uint32_t line, std::string_view message) noexcept {
  try {
    auto* logging = g_active_logging.load(std::memory_order_acquire);
    if (logging) {
      logging->Write(module, level, file, line, message);
    }
  } catch (...) {
  }
}

void WriteFormattedArgs(std::string_view module, LogLevel level,
                        const char* file, std::uint32_t line,
                        fmt::string_view format,
                        fmt::format_args args) noexcept {
  try {
    auto* logging = g_active_logging.load(std::memory_order_acquire);
    if (logging) {
      logging->WriteFormatted(module, level, file, line, format, args);
    }
  } catch (...) {
  }
}

Logging::Logging(const MwLogConfig& config)
    : impl_(std::make_unique<LoggingImpl>(config)) {}

Logging::~Logging() = default;

}  // namespace mw::log

namespace {

std::mutex g_c_logging_mutex;
std::unique_ptr<mw::log::Logging> g_c_logging;

}  // namespace

extern "C" {

void mw_log_default_config(MwLogConfig* config) {
  if (!config) return;
  *config = {};
  config->modules = mw::log::kDefaultModules;
  config->modules_size = sizeof(mw::log::kDefaultModules) - 1;
  config->console_enabled = 1;
  config->console_color = 1;
  config->rotating_file_enabled = 0;
  config->rotating_file_max_size = 10 * 1024 * 1024;
  config->rotating_file_max_files = 5;
  config->async_queue_size = 8192;
  config->async_overflow = kMwLogOverflowOverrunOldest;
}

MwLogResult mw_log_initialize(const MwLogConfig* config) {
  if (!config) return kMwLogInvalidArgument;
  std::lock_guard<std::mutex> lock(g_c_logging_mutex);
  if (g_c_logging) return kMwLogAlreadyInitialized;
  try {
    g_c_logging = std::make_unique<mw::log::Logging>(*config);
    return kMwLogSuccess;
  } catch (const std::invalid_argument&) {
    return kMwLogInvalidArgument;
  } catch (const std::logic_error&) {
    return kMwLogAlreadyInitialized;
  } catch (...) {
    return kMwLogInternalError;
  }
}

void mw_log_shutdown(void) {
  std::lock_guard<std::mutex> lock(g_c_logging_mutex);
  g_c_logging.reset();
}

int mw_log_should_log(MwLogLevel level, const char* module,
                      size_t module_size) {
  mw::log::LogLevel converted;
  if (!mw::log::FromCLevel(level, &converted)) {
    return 0;
  }
  return mw::log::ShouldLog(module ? std::string_view(module, module_size)
                                   : std::string_view{},
                            converted)
             ? 1
             : 0;
}

void mw_log_write(MwLogLevel level, const char* module, size_t module_size,
                  const char* file, uint32_t line, const char* message,
                  size_t message_size) {
  try {
    mw::log::LogLevel converted;
    if (!mw::log::FromCLevel(level, &converted)) {
      return;
    }
    mw::log::Write(
        module ? std::string_view(module, module_size) : std::string_view{},
        converted, file, line,
        message ? std::string_view(message, message_size) : std::string_view{});
  } catch (...) {
  }
}

}  // extern "C"

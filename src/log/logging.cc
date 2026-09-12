#include "mw/log.h"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mw::log {
namespace {

constexpr std::string_view kDefaultModule = "default";
constexpr std::string_view kLogPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";

std::string_view NormalizeModule(std::string_view module) noexcept {
  return module.empty() ? kDefaultModule : module;
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

}  // namespace

class LoggingImpl {
 public:
  explicit LoggingImpl(const LogConfig& config, bool publish = true)
      : default_level_(config.level) {
    for (const auto& module : config.modules) {
      if (module.name.empty()) {
        throw std::invalid_argument("log module name cannot be empty");
      }
      const auto duplicate = std::find_if(
          module_levels_.begin(), module_levels_.end(),
          [&module](const auto& entry) { return entry.first == module.name; });
      if (duplicate != module_levels_.end()) {
        throw std::invalid_argument("duplicate log module: " + module.name);
      }
      module_levels_.emplace_back(module.name, module.level);
    }
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
    const auto configured =
        iterator == module_levels_.end() ? default_level_ : iterator->second;
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

 private:
  void CreateSinks(const LogConfig& config) {
    if (config.console.level != LogLevel::kOff) {
      spdlog::sink_ptr sink;
      if (config.console.color) {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      } else {
        sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      }
      sink->set_level(ToSpdlogLevel(config.console.level));
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (config.rotating_file.level != LogLevel::kOff) {
      if (config.rotating_file.path.empty()) {
        throw std::invalid_argument("rotating log file path cannot be empty");
      }
      if (config.rotating_file.max_file_size == 0 ||
          config.rotating_file.max_files == 0) {
        throw std::invalid_argument(
            "rotating log file size and count must be greater than zero");
      }
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          config.rotating_file.path, config.rotating_file.max_file_size,
          config.rotating_file.max_files);
      sink->set_level(ToSpdlogLevel(config.rotating_file.level));
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }
  }

  void CreateLogger(const LogConfig& config) {
    if (config.async.enabled && !sinks_.empty()) {
      if (config.async.queue_size == 0) {
        throw std::invalid_argument(
            "async log queue size must be greater than zero");
      }
      thread_pool_ = std::make_shared<spdlog::details::thread_pool>(
          config.async.queue_size, 1);
      const auto overflow = config.async.overflow == OverflowPolicy::kBlock
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

  LogLevel default_level_;
  std::vector<std::pair<std::string, LogLevel>> module_levels_;
  std::vector<spdlog::sink_ptr> sinks_;
  std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
  std::shared_ptr<spdlog::logger> logger_;
  bool published_ = false;
};

std::shared_mutex g_logging_mutex;
LoggingImpl* g_active_logging = nullptr;

LoggingImpl& DefaultLogging() {
  static LoggingImpl* logging = [] {
    auto* instance = new LoggingImpl(LogConfig{}, false);
    return instance;
  }();
  return *logging;
}

void LoggingImpl::Publish() {
  std::unique_lock<std::shared_mutex> lock(g_logging_mutex);
  if (g_active_logging) {
    throw std::logic_error("mw log is already initialized");
  }
  g_active_logging = this;
  published_ = true;
}

void LoggingImpl::Shutdown() noexcept {
  if (published_) {
    std::unique_lock<std::shared_mutex> lock(g_logging_mutex);
    if (g_active_logging == this) {
      g_active_logging = nullptr;
    }
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
  std::shared_lock<std::shared_mutex> lock(g_logging_mutex);
  auto* logging = g_active_logging;
  if (logging) {
    return logging->ShouldLog(module, level);
  }
  lock.unlock();
  return DefaultLogging().ShouldLog(module, level);
}

void Write(std::string_view module, LogLevel level, const char* file,
           std::uint32_t line, std::string_view message) {
  std::shared_lock<std::shared_mutex> lock(g_logging_mutex);
  auto* logging = g_active_logging;
  if (logging) {
    logging->Write(module, level, file, line, message);
    return;
  }
  lock.unlock();
  DefaultLogging().Write(module, level, file, line, message);
}

Logging::Logging(const LogConfig& config)
    : impl_(std::make_unique<LoggingImpl>(config)) {}

Logging::~Logging() = default;

}  // namespace mw::log

namespace {

std::mutex g_c_logging_mutex;
std::unique_ptr<mw::log::Logging> g_c_logging;

mw::log::LogConfig ToCppConfig(const MwLogConfig& input) {
  if (input.struct_size < sizeof(MwLogConfig)) {
    throw std::invalid_argument("invalid MwLogConfig size");
  }
  if (input.module_count != 0 && !input.modules) {
    throw std::invalid_argument("log modules cannot be null");
  }

  mw::log::LogConfig output;
  if (!mw::log::FromCLevel(input.level, &output.level) ||
      !mw::log::FromCLevel(input.console_level, &output.console.level) ||
      !mw::log::FromCLevel(input.file_level, &output.rotating_file.level)) {
    throw std::invalid_argument("invalid log level");
  }
  output.console.color = input.console_color != 0;
  if (input.file_path) {
    output.rotating_file.path.assign(input.file_path, input.file_path_size);
  }
  output.rotating_file.max_file_size = input.max_file_size;
  output.rotating_file.max_files = input.max_files;
  output.async.enabled = input.async_enabled != 0;
  output.async.queue_size = input.async_queue_size;
  switch (input.async_overflow) {
    case kMwLogOverflowBlock:
      output.async.overflow = mw::log::OverflowPolicy::kBlock;
      break;
    case kMwLogOverflowOverrunOldest:
      output.async.overflow = mw::log::OverflowPolicy::kOverrunOldest;
      break;
    default:
      throw std::invalid_argument("invalid log overflow policy");
  }
  output.modules.reserve(input.module_count);
  for (std::size_t index = 0; index < input.module_count; ++index) {
    const auto& module = input.modules[index];
    if (!module.name && module.name_size != 0) {
      throw std::invalid_argument("log module name cannot be null");
    }
    mw::log::ModuleLogConfig converted;
    if (module.name) converted.name.assign(module.name, module.name_size);
    if (!mw::log::FromCLevel(module.level, &converted.level)) {
      throw std::invalid_argument("invalid module log level");
    }
    output.modules.emplace_back(std::move(converted));
  }
  return output;
}

}  // namespace

extern "C" {

void mw_log_default_config(MwLogConfig* config) {
  if (!config) return;
  *config = {};
  config->struct_size = sizeof(MwLogConfig);
  config->level = kMwLogLevelInfo;
  config->console_level = kMwLogLevelTrace;
  config->console_color = 1;
  config->file_level = kMwLogLevelOff;
  config->max_file_size = 10 * 1024 * 1024;
  config->max_files = 5;
  config->async_queue_size = 8192;
  config->async_overflow = kMwLogOverflowOverrunOldest;
}

MwLogResult mw_log_initialize(const MwLogConfig* config) {
  if (!config) return kMwLogInvalidArgument;
  std::lock_guard<std::mutex> lock(g_c_logging_mutex);
  if (g_c_logging) return kMwLogAlreadyInitialized;
  try {
    g_c_logging = std::make_unique<mw::log::Logging>(ToCppConfig(*config));
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

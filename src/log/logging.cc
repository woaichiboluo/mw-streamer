#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "log/performance.h"
#include "mw/log.h"

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

std::shared_ptr<spdlog::logger> DefaultLoggerFor(LogLevel level) {
  if (level == LogLevel::kOff) return nullptr;
  auto logger = spdlog::default_logger();
  return logger && logger->should_log(ToSpdlogLevel(level)) ? logger : nullptr;
}

}  // namespace

class LoggingImpl {
 public:
  explicit LoggingImpl(const LogConfig& config) {
    Validate(config);
    modules_.insert(config.modules.begin(), config.modules.end());
    CreateSinks(config);
    CreateLogger(config);
    Publish();
  }

  ~LoggingImpl() { Shutdown(); }

  bool ShouldLog(std::string_view module, LogLevel level) const noexcept {
    if (level == LogLevel::kOff) {
      return false;
    }
    const auto normalized = NormalizeModule(module);
    const auto iterator = modules_.find(normalized);
    const auto configured =
        iterator == modules_.end() ? LogLevel::kInfo : iterator->second;
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
    WriteFormattedUnchecked(module, level, file, line, format, args);
  }

  void WriteFormattedUnchecked(std::string_view label, LogLevel level,
                               const char* file, std::uint32_t line,
                               fmt::string_view format, fmt::format_args args) {
    logger_->log(ToSpdlogLevel(level), "[{}] {} [{}:{}]",
                 NormalizeModule(label), fmt::vformat(format, args),
                 FileName(file), line);
  }

 private:
  static void Validate(const LogConfig& config) {
    if (config.async.overflow != OverflowPolicy::kBlock &&
        config.async.overflow != OverflowPolicy::kOverrunOldest) {
      throw std::invalid_argument("invalid log overflow policy");
    }
    for (const auto& [name, level] : config.modules) {
      if (name.empty()) {
        throw std::invalid_argument("log module name cannot be empty");
      }
      if (LevelRank(level) > LevelRank(LogLevel::kOff)) {
        throw std::invalid_argument("invalid log module level");
      }
    }
  }

  void CreateSinks(const LogConfig& config) {
    if (config.console.enabled) {
      spdlog::sink_ptr sink;
      if (config.console.color) {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      } else {
        sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      }
      sink->set_level(spdlog::level::trace);
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (config.rotating_file.enabled) {
      if (config.rotating_file.path.empty()) {
        throw std::invalid_argument("rotating log file path cannot be empty");
      }
      if (config.rotating_file.max_size == 0 ||
          config.rotating_file.max_files == 0) {
        throw std::invalid_argument(
            "rotating log file size and count must be greater than zero");
      }
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          config.rotating_file.path, config.rotating_file.max_size,
          config.rotating_file.max_files);
      sink->set_level(spdlog::level::trace);
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

  std::map<std::string, LogLevel, std::less<>> modules_;
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
  try {
    return DefaultLoggerFor(level) != nullptr;
  } catch (...) {
    return false;
  }
}

void Write(std::string_view module, LogLevel level, const char* file,
           std::uint32_t line, std::string_view message) noexcept {
  try {
    auto* logging = g_active_logging.load(std::memory_order_acquire);
    if (logging) {
      logging->Write(module, level, file, line, message);
    } else if (auto logger = DefaultLoggerFor(level)) {
      logger->log(ToSpdlogLevel(level), "[{}] {} [{}:{}]",
                  NormalizeModule(module), message, FileName(file), line);
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
    } else if (auto logger = DefaultLoggerFor(level)) {
      logger->log(ToSpdlogLevel(level), "[{}] {} [{}:{}]",
                  NormalizeModule(module), fmt::vformat(format, args),
                  FileName(file), line);
    }
  } catch (...) {
  }
}

void internal::WritePerformanceFormattedArgs(std::string_view label,
                                             LogLevel level, const char* file,
                                             std::uint32_t line,
                                             fmt::string_view format,
                                             fmt::format_args args) noexcept {
  try {
    if (auto* logging = g_active_logging.load(std::memory_order_acquire)) {
      logging->WriteFormattedUnchecked(label, level, file, line, format, args);
    }
  } catch (...) {
  }
}

Logging::Logging(const LogConfig& config)
    : impl_(std::make_unique<LoggingImpl>(config)) {}

Logging::~Logging() = default;

}  // namespace mw::log

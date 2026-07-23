#include "mw/logging.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <srt/srt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

#include "Util/NoticeCenter.h"
#include "Util/logger.h"

namespace mw::log {
namespace {

constexpr std::size_t kModuleCount = static_cast<std::size_t>(LogModule::Count);
constexpr std::string_view kLogPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";
constexpr std::string_view kZlmChannelName = "mw-spdlog-event";

constexpr std::array<std::string_view, kModuleCount> kModuleNames{
    "ZLM",
    "SRT",
    "FFMPEG",
    "streamer",
};

constexpr std::size_t moduleIndex(LogModule module) noexcept {
  return static_cast<std::size_t>(module);
}

constexpr bool isValidModule(LogModule module) noexcept {
  return moduleIndex(module) < kModuleCount;
}

spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return spdlog::level::trace;
    case LogLevel::Debug:
      return spdlog::level::debug;
    case LogLevel::Info:
      return spdlog::level::info;
    case LogLevel::Warning:
      return spdlog::level::warn;
    case LogLevel::Error:
      return spdlog::level::err;
    case LogLevel::Critical:
      return spdlog::level::critical;
    case LogLevel::Off:
      return spdlog::level::off;
  }
  return spdlog::level::off;
}

int levelRank(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:
      return 0;
    case LogLevel::Debug:
      return 1;
    case LogLevel::Info:
      return 2;
    case LogLevel::Warning:
      return 3;
    case LogLevel::Error:
      return 4;
    case LogLevel::Critical:
      return 5;
    case LogLevel::Off:
      return 6;
  }
  return 6;
}

toolkit::LogLevel toZlmLevel(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:
      return toolkit::LTrace;
    case LogLevel::Debug:
      return toolkit::LDebug;
    case LogLevel::Info:
      return toolkit::LInfo;
    case LogLevel::Warning:
      return toolkit::LWarn;
    case LogLevel::Error:
    case LogLevel::Critical:
    case LogLevel::Off:
      return toolkit::LError;
  }
  return toolkit::LError;
}

LogLevel fromZlmLevel(toolkit::LogLevel level) noexcept {
  switch (level) {
    case toolkit::LTrace:
      return LogLevel::Trace;
    case toolkit::LDebug:
      return LogLevel::Debug;
    case toolkit::LInfo:
      return LogLevel::Info;
    case toolkit::LWarn:
      return LogLevel::Warning;
    case toolkit::LError:
      return LogLevel::Error;
  }
  return LogLevel::Error;
}

int toSrtLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug:
      return LOG_DEBUG;
    case LogLevel::Info:
      return LOG_NOTICE;
    case LogLevel::Warning:
      return LOG_WARNING;
    case LogLevel::Error:
      return LOG_ERR;
    case LogLevel::Critical:
    case LogLevel::Off:
      return LOG_CRIT;
  }
  return LOG_CRIT;
}

LogLevel fromSrtLevel(int level) noexcept {
  if (level <= LOG_CRIT) {
    return LogLevel::Critical;
  }
  if (level <= LOG_ERR) {
    return LogLevel::Error;
  }
  if (level <= LOG_WARNING) {
    return LogLevel::Warning;
  }
  if (level <= LOG_NOTICE) {
    return LogLevel::Info;
  }
  return LogLevel::Debug;
}

int toFfmpegLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:
      return AV_LOG_TRACE;
    case LogLevel::Debug:
      return AV_LOG_DEBUG;
    case LogLevel::Info:
      return AV_LOG_INFO;
    case LogLevel::Warning:
      return AV_LOG_WARNING;
    case LogLevel::Error:
      return AV_LOG_ERROR;
    case LogLevel::Critical:
      return AV_LOG_FATAL;
    case LogLevel::Off:
      return AV_LOG_QUIET;
  }
  return AV_LOG_QUIET;
}

LogLevel fromFfmpegLevel(int level) noexcept {
  if (level <= AV_LOG_FATAL) {
    return LogLevel::Critical;
  }
  if (level <= AV_LOG_ERROR) {
    return LogLevel::Error;
  }
  if (level <= AV_LOG_WARNING) {
    return LogLevel::Warning;
  }
  if (level <= AV_LOG_INFO) {
    return LogLevel::Info;
  }
  if (level <= AV_LOG_DEBUG) {
    return LogLevel::Debug;
  }
  return LogLevel::Trace;
}

std::string_view trimLineEnd(std::string_view message) noexcept {
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.remove_suffix(1);
  }
  return message;
}

std::array<LogLevel, kModuleCount> moduleLevels(const LogModuleConfig& config) {
  return {
      config.zlm,
      config.srt,
      config.ffmpeg,
      config.streamer,
  };
}

std::uint64_t nextLoggingGeneration() noexcept {
  static std::atomic<std::uint64_t> generation{0};
  return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

namespace detail {

class LoggingImpl {
 public:
  LoggingImpl(const LogConfig& config, bool install_bridges)
      : generation_(nextLoggingGeneration()),
        module_levels_(moduleLevels(config.modules)) {
    createSinks(config);
    createLogger(config);
    if (!install_bridges) {
      return;
    }
    publish();
    try {
      installZlmBridge();
      installSrtBridge();
      installFfmpegBridge();
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~LoggingImpl() { shutdown(); }

  LoggingImpl(const LoggingImpl&) = delete;
  LoggingImpl& operator=(const LoggingImpl&) = delete;

  bool shouldLog(LogModule module, LogLevel level) const noexcept {
    if (!isValidModule(module) || level == LogLevel::Off) {
      return false;
    }
    const auto configured = module_levels_[moduleIndex(module)];
    return configured != LogLevel::Off &&
           levelRank(level) >= levelRank(configured);
  }

  void write(LogModule module, LogLevel level, std::string_view message) {
    if (!shouldLog(module, level)) {
      return;
    }
    logger_->log(toSpdlogLevel(level), "[{}] {}",
                 kModuleNames[moduleIndex(module)], message);
  }

 private:
  void createSinks(const LogConfig& config) {
    if (config.console.enabled) {
      spdlog::sink_ptr sink;
      if (config.console.color) {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      } else {
        sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      }
      sink->set_level(toSpdlogLevel(config.console.level));
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (config.rotating_file.enabled) {
      if (config.rotating_file.path.empty()) {
        throw std::invalid_argument("rotating log file path cannot be empty");
      }
      if (config.rotating_file.max_file_size == 0) {
        throw std::invalid_argument(
            "rotating log file max size must be greater than zero");
      }
      if (config.rotating_file.max_files == 0) {
        throw std::invalid_argument(
            "rotating log file count must be greater than zero");
      }

      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          config.rotating_file.path, config.rotating_file.max_file_size,
          config.rotating_file.max_files);
      sink->set_level(toSpdlogLevel(config.rotating_file.level));
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (sinks_.empty()) {
      throw std::invalid_argument("at least one log sink must be enabled");
    }
  }

  void createLogger(const LogConfig& config) {
    if (config.async.enabled) {
      if (config.async.queue_size == 0) {
        throw std::invalid_argument(
            "async log queue size must be greater than zero");
      }
      thread_pool_ = std::make_shared<spdlog::details::thread_pool>(
          config.async.queue_size, 1);
      const auto overflow = config.async.overflow == OverflowPolicy::Block
                                ? spdlog::async_overflow_policy::block
                                : spdlog::async_overflow_policy::overrun_oldest;
      logger_ = std::make_shared<spdlog::async_logger>(
          "mw-streamer", sinks_.begin(), sinks_.end(), thread_pool_, overflow);
    } else {
      logger_ = std::make_shared<spdlog::logger>("mw-streamer", sinks_.begin(),
                                                 sinks_.end());
    }
    logger_->set_level(spdlog::level::trace);
  }

  void publish();
  void unpublish() noexcept;

  void installZlmBridge() {
    auto& zlm_logger = toolkit::getLogger();
    if (zlm_logger.get(std::string(kZlmChannelName))) {
      throw std::logic_error("ZLM log bridge is already installed");
    }

    toolkit::NoticeCenter::Instance().addListener(
        this, toolkit::EventChannel::getBroadcastLogEventName(),
        [](const toolkit::Logger&, const toolkit::LogContextPtr& context) {
          try {
            const auto level = fromZlmLevel(context->_level);
            if (!detail::shouldLog(LogModule::Zlm, level)) {
              return;
            }
            detail::write(LogModule::Zlm, level, context->str());
            if (context->_repeat > 1) {
              detail::write(LogModule::Zlm, level,
                            fmt::format("last message repeated {} times",
                                        context->_repeat));
            }
          } catch (...) {
          }
        });
    zlm_listener_installed_ = true;

    zlm_channel_ = std::make_shared<toolkit::EventChannel>(
        std::string(kZlmChannelName),
        toZlmLevel(module_levels_[moduleIndex(LogModule::Zlm)]));
    zlm_logger.add(zlm_channel_);
    zlm_channel_installed_ = true;
  }

  void installSrtBridge() noexcept {
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME |
                    SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
    srt_setloglevel(toSrtLevel(module_levels_[moduleIndex(LogModule::Srt)]));
    srt_setloghandler(nullptr, &LoggingImpl::srtLogCallback);
    srt_bridge_installed_ = true;
  }

  void installFfmpegBridge() noexcept {
    previous_ffmpeg_level_ = av_log_get_level();
    av_log_set_level(
        toFfmpegLevel(module_levels_[moduleIndex(LogModule::Ffmpeg)]));
    av_log_set_callback(&LoggingImpl::ffmpegLogCallback);
    ffmpeg_bridge_installed_ = true;
  }

  void uninstallZlmBridge() noexcept {
    if (zlm_channel_installed_) {
      toolkit::getLogger().del(std::string(kZlmChannelName));
      zlm_channel_installed_ = false;
      zlm_channel_.reset();
    }
    if (zlm_listener_installed_) {
      toolkit::NoticeCenter::Instance().delListener(
          this, toolkit::EventChannel::getBroadcastLogEventName());
      zlm_listener_installed_ = false;
    }
  }

  void uninstallSrtBridge() noexcept {
    if (!srt_bridge_installed_) {
      return;
    }
    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);
    srt_setlogflags(0);
    srt_bridge_installed_ = false;
  }

  void uninstallFfmpegBridge() noexcept {
    if (!ffmpeg_bridge_installed_) {
      return;
    }
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(previous_ffmpeg_level_);
    ffmpeg_bridge_installed_ = false;
  }

  void shutdown() noexcept {
    uninstallFfmpegBridge();
    uninstallSrtBridge();
    uninstallZlmBridge();
    unpublish();

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

  static void srtLogCallback(void* opaque, int level, const char* file,
                             int line, const char* area,
                             const char* message) noexcept {
    try {
      (void)opaque;
      const auto mapped_level = fromSrtLevel(level);
      if (!detail::shouldLog(LogModule::Srt, mapped_level)) {
        return;
      }

      auto text =
          trimLineEnd(message ? std::string_view(message) : std::string_view{});
      if (text.size() >= 2 && text.substr(0, 2) == ": ") {
        text.remove_prefix(2);
      }
      detail::write(LogModule::Srt, mapped_level,
                    fmt::format("[{}] {} ({}:{})", area ? area : "SRT", text,
                                file ? file : "unknown", line));
    } catch (...) {
    }
  }

  static void ffmpegLogCallback(void* context, int level, const char* format,
                                va_list arguments) noexcept;

 private:
  std::uint64_t generation_;
  std::array<LogLevel, kModuleCount> module_levels_;
  int previous_ffmpeg_level_ = AV_LOG_INFO;
  std::vector<spdlog::sink_ptr> sinks_;
  std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
  std::shared_ptr<spdlog::logger> logger_;
  std::shared_ptr<toolkit::EventChannel> zlm_channel_;
  bool zlm_listener_installed_ = false;
  bool zlm_channel_installed_ = false;
  bool srt_bridge_installed_ = false;
  bool ffmpeg_bridge_installed_ = false;
  bool published_ = false;
};

std::shared_mutex g_active_logging_mutex;
LoggingImpl* g_active_logging = nullptr;

LoggingImpl& defaultLogging() {
  static LoggingImpl logging(LogConfig{}, false);
  return logging;
}

void LoggingImpl::publish() {
  std::unique_lock<std::shared_mutex> lock(g_active_logging_mutex);
  if (g_active_logging) {
    throw std::logic_error("mw logging is already initialized");
  }
  g_active_logging = this;
  published_ = true;
}

void LoggingImpl::unpublish() noexcept {
  if (!published_) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(g_active_logging_mutex);
  if (g_active_logging == this) {
    g_active_logging = nullptr;
  }
  published_ = false;
}

void LoggingImpl::ffmpegLogCallback(void* context, int level,
                                    const char* format,
                                    va_list arguments) noexcept {
  try {
    std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
    auto* self = g_active_logging;
    if (!self) {
      return;
    }

    const auto mapped_level = fromFfmpegLevel(level);
    if (!self->shouldLog(LogModule::Ffmpeg, mapped_level)) {
      return;
    }

    thread_local int print_prefix = 1;
    thread_local std::string pending;
    thread_local std::uint64_t generation = 0;

    if (generation != self->generation_) {
      print_prefix = 1;
      pending.clear();
      generation = self->generation_;
    }

    std::array<char, 2048> stack_buffer{};
    auto next_print_prefix = print_prefix;
    va_list copy;
    va_copy(copy, arguments);
    const auto required = av_log_format_line2(
        context, level, format, copy, stack_buffer.data(),
        static_cast<int>(stack_buffer.size()), &next_print_prefix);
    va_end(copy);
    if (required < 0) {
      return;
    }

    std::string formatted;
    if (static_cast<std::size_t>(required) < stack_buffer.size()) {
      formatted.assign(stack_buffer.data(), static_cast<std::size_t>(required));
      print_prefix = next_print_prefix;
    } else {
      formatted.resize(static_cast<std::size_t>(required) + 1);
      auto replay_print_prefix = print_prefix;
      va_copy(copy, arguments);
      const auto replayed = av_log_format_line2(
          context, level, format, copy, formatted.data(),
          static_cast<int>(formatted.size()), &replay_print_prefix);
      va_end(copy);
      if (replayed < 0) {
        return;
      }
      formatted.resize(static_cast<std::size_t>(replayed));
      print_prefix = replay_print_prefix;
    }

    pending.append(formatted);
    std::size_t line_start = 0;
    while (true) {
      const auto line_end = pending.find('\n', line_start);
      if (line_end == std::string::npos) {
        pending.erase(0, line_start);
        break;
      }
      auto line = trimLineEnd(
          std::string_view(pending).substr(line_start, line_end - line_start));
      if (!line.empty()) {
        self->write(LogModule::Ffmpeg, mapped_level, line);
      }
      line_start = line_end + 1;
    }
  } catch (...) {
  }
}

bool shouldLog(LogModule module, LogLevel level) noexcept {
  std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
  auto* logging = g_active_logging;
  return (logging ? *logging : defaultLogging()).shouldLog(module, level);
}

void write(LogModule module, LogLevel level, std::string_view message) {
  std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
  auto* logging = g_active_logging;
  (logging ? *logging : defaultLogging()).write(module, level, message);
}

}  // namespace detail

Logging::Logging(const LogConfig& config)
    : impl_(std::make_unique<detail::LoggingImpl>(config, true)) {}

Logging::~Logging() = default;

}  // namespace mw::log

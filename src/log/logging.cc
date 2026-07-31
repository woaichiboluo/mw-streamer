#include "mw/log/logging.h"

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

namespace mw::streamer::log {
namespace {

constexpr std::size_t kModuleCount =
    static_cast<std::size_t>(LogModule::kCount);
constexpr std::string_view kLogPattern =
    "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v";
constexpr std::string_view kZlmChannelName = "mw-spdlog-event";

constexpr std::array<std::string_view, kModuleCount> kModuleNames{
    "ZLM", "SRT", "FFMPEG", "streamer", "processor",
};

constexpr std::size_t ModuleIndex(LogModule module) noexcept {
  return static_cast<std::size_t>(module);
}

constexpr bool IsValidModule(LogModule module) noexcept {
  return ModuleIndex(module) < kModuleCount;
}

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
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

int LevelRank(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return 0;
    case LogLevel::kDebug:
      return 1;
    case LogLevel::kInfo:
      return 2;
    case LogLevel::kWarning:
      return 3;
    case LogLevel::kError:
      return 4;
    case LogLevel::kCritical:
      return 5;
    case LogLevel::kOff:
      return 6;
  }
  return 6;
}

toolkit::LogLevel ToZlmLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return toolkit::LTrace;
    case LogLevel::kDebug:
      return toolkit::LDebug;
    case LogLevel::kInfo:
      return toolkit::LInfo;
    case LogLevel::kWarning:
      return toolkit::LWarn;
    case LogLevel::kError:
    case LogLevel::kCritical:
    case LogLevel::kOff:
      return toolkit::LError;
  }
  return toolkit::LError;
}

LogLevel FromZlmLevel(toolkit::LogLevel level) noexcept {
  switch (level) {
    case toolkit::LTrace:
      return LogLevel::kTrace;
    case toolkit::LDebug:
      return LogLevel::kDebug;
    case toolkit::LInfo:
      return LogLevel::kInfo;
    case toolkit::LWarn:
      return LogLevel::kWarning;
    case toolkit::LError:
      return LogLevel::kError;
  }
  return LogLevel::kError;
}

int ToSrtLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
    case LogLevel::kDebug:
      return LOG_DEBUG;
    case LogLevel::kInfo:
      return LOG_NOTICE;
    case LogLevel::kWarning:
      return LOG_WARNING;
    case LogLevel::kError:
      return LOG_ERR;
    case LogLevel::kCritical:
    case LogLevel::kOff:
      return LOG_CRIT;
  }
  return LOG_CRIT;
}

LogLevel FromSrtLevel(int level) noexcept {
  if (level <= LOG_CRIT) {
    return LogLevel::kCritical;
  }
  if (level <= LOG_ERR) {
    return LogLevel::kError;
  }
  if (level <= LOG_WARNING) {
    return LogLevel::kWarning;
  }
  if (level <= LOG_NOTICE) {
    return LogLevel::kInfo;
  }
  return LogLevel::kDebug;
}

int ToFfmpegLevel(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return AV_LOG_TRACE;
    case LogLevel::kDebug:
      return AV_LOG_DEBUG;
    case LogLevel::kInfo:
      return AV_LOG_INFO;
    case LogLevel::kWarning:
      return AV_LOG_WARNING;
    case LogLevel::kError:
      return AV_LOG_ERROR;
    case LogLevel::kCritical:
      return AV_LOG_FATAL;
    case LogLevel::kOff:
      return AV_LOG_QUIET;
  }
  return AV_LOG_QUIET;
}

LogLevel FromFfmpegLevel(int level) noexcept {
  if (level <= AV_LOG_FATAL) {
    return LogLevel::kCritical;
  }
  if (level <= AV_LOG_ERROR) {
    return LogLevel::kError;
  }
  if (level <= AV_LOG_WARNING) {
    return LogLevel::kWarning;
  }
  if (level <= AV_LOG_INFO) {
    return LogLevel::kInfo;
  }
  if (level <= AV_LOG_DEBUG) {
    return LogLevel::kDebug;
  }
  return LogLevel::kTrace;
}

std::string_view TrimLineEnd(std::string_view message) noexcept {
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.remove_suffix(1);
  }
  return message;
}

std::array<LogLevel, kModuleCount> ModuleLevels(const LogModuleConfig& config) {
  return {
      config.zlm, config.srt, config.ffmpeg, config.streamer, config.processor,
  };
}

std::uint64_t NextLoggingGeneration() noexcept {
  static std::atomic<std::uint64_t> generation{0};
  return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

namespace detail {

class LoggingImpl {
 public:
  LoggingImpl(const LogConfig& config, bool install_bridges)
      : generation_(NextLoggingGeneration()),
        module_levels_(ModuleLevels(config.modules)) {
    CreateSinks(config);
    CreateLogger(config);
    if (!install_bridges) {
      return;
    }
    Publish();
    try {
      InstallZlmBridge();
      InstallSrtBridge();
      InstallFfmpegBridge();
    } catch (...) {
      Shutdown();
      throw;
    }
  }

  ~LoggingImpl() { Shutdown(); }

  LoggingImpl(const LoggingImpl&) = delete;
  LoggingImpl& operator=(const LoggingImpl&) = delete;

  bool ShouldLog(LogModule module, LogLevel level) const noexcept {
    if (!IsValidModule(module) || level == LogLevel::kOff) {
      return false;
    }
    const auto configured = module_levels_[ModuleIndex(module)];
    return configured != LogLevel::kOff &&
           LevelRank(level) >= LevelRank(configured);
  }

  void Write(LogModule module, LogLevel level, std::string_view message) {
    if (!ShouldLog(module, level)) {
      return;
    }
    logger_->log(ToSpdlogLevel(level), "[{}] {}",
                 kModuleNames[ModuleIndex(module)], message);
  }

 private:
  void CreateSinks(const LogConfig& config) {
    if (config.console.enabled) {
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
      sink->set_level(ToSpdlogLevel(config.rotating_file.level));
      sink->set_pattern(std::string(kLogPattern));
      sinks_.emplace_back(std::move(sink));
    }

    if (sinks_.empty()) {
      throw std::invalid_argument("at least one log sink must be enabled");
    }
  }

  void CreateLogger(const LogConfig& config) {
    if (config.async.enabled) {
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
          "mw-streamer", sinks_.begin(), sinks_.end(), thread_pool_, overflow);
    } else {
      logger_ = std::make_shared<spdlog::logger>("mw-streamer", sinks_.begin(),
                                                 sinks_.end());
    }
    logger_->set_level(spdlog::level::trace);
  }

  void Publish();
  void Unpublish() noexcept;

  void InstallZlmBridge() {
    auto& zlm_logger = toolkit::getLogger();
    if (zlm_logger.get(std::string(kZlmChannelName))) {
      throw std::logic_error("ZLM log bridge is already installed");
    }

    toolkit::NoticeCenter::Instance().addListener(
        this, toolkit::EventChannel::getBroadcastLogEventName(),
        [](const toolkit::Logger&, const toolkit::LogContextPtr& context) {
          try {
            const auto level = FromZlmLevel(context->_level);
            if (!detail::ShouldLog(LogModule::kZlm, level)) {
              return;
            }
            detail::Write(LogModule::kZlm, level, context->str());
            if (context->_repeat > 1) {
              detail::Write(LogModule::kZlm, level,
                            fmt::format("last message repeated {} times",
                                        context->_repeat));
            }
          } catch (...) {
          }
        });
    zlm_listener_installed_ = true;

    zlm_channel_ = std::make_shared<toolkit::EventChannel>(
        std::string(kZlmChannelName),
        ToZlmLevel(module_levels_[ModuleIndex(LogModule::kZlm)]));
    zlm_logger.add(zlm_channel_);
    zlm_channel_installed_ = true;
  }

  void InstallSrtBridge() noexcept {
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME |
                    SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
    srt_setloglevel(ToSrtLevel(module_levels_[ModuleIndex(LogModule::kSrt)]));
    srt_setloghandler(nullptr, &LoggingImpl::SrtLogCallback);
    srt_bridge_installed_ = true;
  }

  void InstallFfmpegBridge() noexcept {
    previous_ffmpeg_level_ = av_log_get_level();
    av_log_set_level(
        ToFfmpegLevel(module_levels_[ModuleIndex(LogModule::kFfmpeg)]));
    av_log_set_callback(&LoggingImpl::FfmpegLogCallback);
    ffmpeg_bridge_installed_ = true;
  }

  void UninstallZlmBridge() noexcept {
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

  void UninstallSrtBridge() noexcept {
    if (!srt_bridge_installed_) {
      return;
    }
    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);
    srt_setlogflags(0);
    srt_bridge_installed_ = false;
  }

  void UninstallFfmpegBridge() noexcept {
    if (!ffmpeg_bridge_installed_) {
      return;
    }
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(previous_ffmpeg_level_);
    ffmpeg_bridge_installed_ = false;
  }

  void Shutdown() noexcept {
    UninstallFfmpegBridge();
    UninstallSrtBridge();
    UninstallZlmBridge();
    Unpublish();

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

  static void SrtLogCallback(void* opaque, int level, const char* file,
                             int line, const char* area,
                             const char* message) noexcept {
    try {
      (void)opaque;
      const auto mapped_level = FromSrtLevel(level);
      if (!detail::ShouldLog(LogModule::kSrt, mapped_level)) {
        return;
      }

      auto text =
          TrimLineEnd(message ? std::string_view(message) : std::string_view{});
      if (text.size() >= 2 && text.substr(0, 2) == ": ") {
        text.remove_prefix(2);
      }
      detail::Write(LogModule::kSrt, mapped_level,
                    fmt::format("[{}] {} ({}:{})", area ? area : "SRT", text,
                                file ? file : "unknown", line));
    } catch (...) {
    }
  }

  static void FfmpegLogCallback(void* context, int level, const char* format,
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

LoggingImpl& DefaultLogging() {
  static LoggingImpl logging(LogConfig{}, false);
  return logging;
}

void LoggingImpl::Publish() {
  std::unique_lock<std::shared_mutex> lock(g_active_logging_mutex);
  if (g_active_logging) {
    throw std::logic_error("mw logging is already initialized");
  }
  g_active_logging = this;
  published_ = true;
}

void LoggingImpl::Unpublish() noexcept {
  if (!published_) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(g_active_logging_mutex);
  if (g_active_logging == this) {
    g_active_logging = nullptr;
  }
  published_ = false;
}

void LoggingImpl::FfmpegLogCallback(void* context, int level,
                                    const char* format,
                                    va_list arguments) noexcept {
  try {
    std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
    auto* self = g_active_logging;
    if (!self) {
      return;
    }

    const auto mapped_level = FromFfmpegLevel(level);
    if (!self->ShouldLog(LogModule::kFfmpeg, mapped_level)) {
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
      auto line = TrimLineEnd(
          std::string_view(pending).substr(line_start, line_end - line_start));
      if (!line.empty()) {
        self->Write(LogModule::kFfmpeg, mapped_level, line);
      }
      line_start = line_end + 1;
    }
  } catch (...) {
  }
}

bool ShouldLog(LogModule module, LogLevel level) noexcept {
  std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
  auto* logging = g_active_logging;
  return (logging ? *logging : DefaultLogging()).ShouldLog(module, level);
}

void Write(LogModule module, LogLevel level, std::string_view message) {
  std::shared_lock<std::shared_mutex> lock(g_active_logging_mutex);
  auto* logging = g_active_logging;
  (logging ? *logging : DefaultLogging()).Write(module, level, message);
}

}  // namespace detail

Logging::Logging(const LogConfig& config)
    : impl_(std::make_unique<detail::LoggingImpl>(config, true)) {}

Logging::~Logging() = default;

}  // namespace mw::streamer::log

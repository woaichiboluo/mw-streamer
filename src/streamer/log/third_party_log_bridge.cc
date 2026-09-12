#include "mw/streamer/log/internal/third_party_log_bridge.h"

#include <srt/srt.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <libavutil/log.h>
}

#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

constexpr std::string_view kZlmModule = "zlm";
constexpr std::string_view kSrtModule = "srt";
constexpr std::string_view kFfmpegModule = "ffmpeg";
constexpr std::string_view kZlmChannelName = "mw-log-event";

mw::log::LogLevel FromZlmLevel(toolkit::LogLevel level) noexcept {
  switch (level) {
    case toolkit::LTrace:
      return mw::log::LogLevel::kTrace;
    case toolkit::LDebug:
      return mw::log::LogLevel::kDebug;
    case toolkit::LInfo:
      return mw::log::LogLevel::kInfo;
    case toolkit::LWarn:
      return mw::log::LogLevel::kWarning;
    case toolkit::LError:
      return mw::log::LogLevel::kError;
  }
  return mw::log::LogLevel::kError;
}

mw::log::LogLevel FromSrtLevel(int level) noexcept {
  if (level <= LOG_CRIT) return mw::log::LogLevel::kCritical;
  if (level <= LOG_ERR) return mw::log::LogLevel::kError;
  if (level <= LOG_WARNING) return mw::log::LogLevel::kWarning;
  if (level <= LOG_NOTICE) return mw::log::LogLevel::kInfo;
  return mw::log::LogLevel::kDebug;
}

mw::log::LogLevel FromFfmpegLevel(int level) noexcept {
  if (level <= AV_LOG_FATAL) return mw::log::LogLevel::kCritical;
  if (level <= AV_LOG_ERROR) return mw::log::LogLevel::kError;
  if (level <= AV_LOG_WARNING) return mw::log::LogLevel::kWarning;
  if (level <= AV_LOG_INFO) return mw::log::LogLevel::kInfo;
  if (level <= AV_LOG_DEBUG) return mw::log::LogLevel::kDebug;
  return mw::log::LogLevel::kTrace;
}

mw::log::LogLevel EffectiveLevel(std::string_view module) noexcept {
  constexpr std::array levels{
      mw::log::LogLevel::kTrace, mw::log::LogLevel::kDebug,
      mw::log::LogLevel::kInfo,  mw::log::LogLevel::kWarning,
      mw::log::LogLevel::kError, mw::log::LogLevel::kCritical};
  for (const auto level : levels) {
    if (mw::log::ShouldLog(module, level)) return level;
  }
  return mw::log::LogLevel::kOff;
}

toolkit::LogLevel ToZlmLevel(mw::log::LogLevel level) noexcept {
  switch (level) {
    case mw::log::LogLevel::kTrace:
      return toolkit::LTrace;
    case mw::log::LogLevel::kDebug:
      return toolkit::LDebug;
    case mw::log::LogLevel::kInfo:
      return toolkit::LInfo;
    case mw::log::LogLevel::kWarning:
      return toolkit::LWarn;
    default:
      return toolkit::LError;
  }
}

int ToSrtLevel(mw::log::LogLevel level) noexcept {
  switch (level) {
    case mw::log::LogLevel::kTrace:
    case mw::log::LogLevel::kDebug:
      return LOG_DEBUG;
    case mw::log::LogLevel::kInfo:
      return LOG_NOTICE;
    case mw::log::LogLevel::kWarning:
      return LOG_WARNING;
    case mw::log::LogLevel::kError:
      return LOG_ERR;
    default:
      return LOG_CRIT;
  }
}

int ToFfmpegLevel(mw::log::LogLevel level) noexcept {
  switch (level) {
    case mw::log::LogLevel::kTrace:
      return AV_LOG_TRACE;
    case mw::log::LogLevel::kDebug:
      return AV_LOG_DEBUG;
    case mw::log::LogLevel::kInfo:
      return AV_LOG_INFO;
    case mw::log::LogLevel::kWarning:
      return AV_LOG_WARNING;
    case mw::log::LogLevel::kError:
      return AV_LOG_ERROR;
    case mw::log::LogLevel::kCritical:
      return AV_LOG_FATAL;
    case mw::log::LogLevel::kOff:
      return AV_LOG_QUIET;
  }
  return AV_LOG_QUIET;
}

std::string_view TrimLineEnd(std::string_view message) noexcept {
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.remove_suffix(1);
  }
  return message;
}

std::uint64_t NextGeneration() noexcept {
  static std::atomic<std::uint64_t> generation{0};
  return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace

class ThirdPartyLogBridge::Impl {
 public:
  Impl() : generation_(NextGeneration()) {
    try {
      InstallZlm();
      InstallSrt();
      InstallFfmpeg();
    } catch (...) {
      UninstallFfmpeg();
      UninstallSrt();
      UninstallZlm();
      throw;
    }
  }

  ~Impl() {
    UninstallFfmpeg();
    UninstallSrt();
    UninstallZlm();
  }

 private:
  void InstallZlm() {
    auto& logger = toolkit::getLogger();
    if (logger.get(std::string(kZlmChannelName))) {
      throw std::logic_error("ZLM log bridge is already installed");
    }
    toolkit::NoticeCenter::Instance().addListener(
        this, toolkit::EventChannel::getBroadcastLogEventName(),
        [](const toolkit::Logger&, const toolkit::LogContextPtr& context) {
          try {
            const auto level = FromZlmLevel(context->_level);
            if (!mw::log::ShouldLog(kZlmModule, level)) return;
            mw::log::Write(kZlmModule, level, context->_file.c_str(),
                           static_cast<std::uint32_t>(context->_line),
                           context->str());
            if (context->_repeat > 1) {
              mw::log::Write(kZlmModule, level, context->_file.c_str(),
                             static_cast<std::uint32_t>(context->_line),
                             fmt::format("last message repeated {} times",
                                         context->_repeat));
            }
          } catch (...) {
          }
        });
    listener_installed_ = true;
    channel_ = std::make_shared<toolkit::EventChannel>(
        std::string(kZlmChannelName), ToZlmLevel(EffectiveLevel(kZlmModule)));
    logger.add(channel_);
    channel_installed_ = true;
  }

  void InstallSrt() noexcept {
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME |
                    SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
    srt_setloglevel(ToSrtLevel(EffectiveLevel(kSrtModule)));
    srt_setloghandler(this, &Impl::SrtCallback);
    srt_installed_ = true;
  }

  void InstallFfmpeg() {
    std::unique_lock<std::shared_mutex> lock(g_bridge_mutex_);
    if (g_bridge_) throw std::logic_error("FFmpeg log bridge is installed");
    previous_ffmpeg_level_ = av_log_get_level();
    g_bridge_ = this;
    av_log_set_level(ToFfmpegLevel(EffectiveLevel(kFfmpegModule)));
    av_log_set_callback(&Impl::FfmpegCallback);
    ffmpeg_installed_ = true;
  }

  void UninstallZlm() noexcept {
    if (channel_installed_) {
      toolkit::getLogger().del(std::string(kZlmChannelName));
      channel_.reset();
      channel_installed_ = false;
    }
    if (listener_installed_) {
      toolkit::NoticeCenter::Instance().delListener(
          this, toolkit::EventChannel::getBroadcastLogEventName());
      listener_installed_ = false;
    }
  }

  void UninstallSrt() noexcept {
    if (!srt_installed_) return;
    srt_setloghandler(nullptr, nullptr);
    srt_setloglevel(LOG_WARNING);
    srt_setlogflags(0);
    srt_installed_ = false;
  }

  void UninstallFfmpeg() noexcept {
    if (!ffmpeg_installed_) return;
    std::unique_lock<std::shared_mutex> lock(g_bridge_mutex_);
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(previous_ffmpeg_level_);
    if (g_bridge_ == this) g_bridge_ = nullptr;
    ffmpeg_installed_ = false;
  }

  static void SrtCallback(void*, int level, const char* file, int line,
                          const char* area, const char* message) noexcept {
    try {
      const auto mapped = FromSrtLevel(level);
      if (!mw::log::ShouldLog(kSrtModule, mapped)) return;
      auto text =
          TrimLineEnd(message ? std::string_view(message) : std::string_view{});
      if (text.substr(0, 2) == ": ") text.remove_prefix(2);
      mw::log::Write(kSrtModule, mapped, file ? file : "srt",
                     static_cast<uint32_t>(line),
                     fmt::format("[{}] {}", area ? area : "SRT", text));
    } catch (...) {
    }
  }

  static void FfmpegCallback(void* context, int level, const char* format,
                             va_list arguments) noexcept {
    try {
      std::shared_lock<std::shared_mutex> lock(g_bridge_mutex_);
      if (g_bridge_) g_bridge_->WriteFfmpeg(context, level, format, arguments);
    } catch (...) {
    }
  }

  void WriteFfmpeg(void* context, int level, const char* format,
                   va_list arguments) {
    const auto mapped = FromFfmpegLevel(level);
    if (!mw::log::ShouldLog(kFfmpegModule, mapped)) return;
    thread_local int print_prefix = 1;
    thread_local std::string pending;
    thread_local std::uint64_t generation = 0;
    if (generation != generation_) {
      print_prefix = 1;
      pending.clear();
      generation = generation_;
    }
    std::array<char, 2048> buffer{};
    auto next_prefix = print_prefix;
    va_list copy;
    va_copy(copy, arguments);
    const auto required =
        av_log_format_line2(context, level, format, copy, buffer.data(),
                            static_cast<int>(buffer.size()), &next_prefix);
    va_end(copy);
    if (required < 0) return;
    std::string formatted;
    if (static_cast<std::size_t>(required) < buffer.size()) {
      formatted.assign(buffer.data(), static_cast<std::size_t>(required));
    } else {
      formatted.resize(static_cast<std::size_t>(required) + 1);
      auto replay_prefix = print_prefix;
      va_copy(copy, arguments);
      const auto replayed = av_log_format_line2(
          context, level, format, copy, formatted.data(),
          static_cast<int>(formatted.size()), &replay_prefix);
      va_end(copy);
      if (replayed < 0) return;
      formatted.resize(static_cast<std::size_t>(replayed));
      next_prefix = replay_prefix;
    }
    print_prefix = next_prefix;
    pending.append(formatted);
    while (true) {
      const auto end = pending.find('\n');
      if (end == std::string::npos) break;
      auto line = TrimLineEnd(std::string_view(pending).substr(0, end));
      if (!line.empty()) {
        mw::log::Write(kFfmpegModule, mapped, __FILE__, __LINE__, line);
      }
      pending.erase(0, end + 1);
    }
  }

  inline static std::shared_mutex g_bridge_mutex_;
  inline static Impl* g_bridge_ = nullptr;
  std::uint64_t generation_;
  int previous_ffmpeg_level_ = AV_LOG_INFO;
  std::shared_ptr<toolkit::EventChannel> channel_;
  bool listener_installed_ = false;
  bool channel_installed_ = false;
  bool srt_installed_ = false;
  bool ffmpeg_installed_ = false;
};

ThirdPartyLogBridge::ThirdPartyLogBridge() : impl_(std::make_unique<Impl>()) {}
ThirdPartyLogBridge::~ThirdPartyLogBridge() = default;

}  // namespace mw::streamer::internal

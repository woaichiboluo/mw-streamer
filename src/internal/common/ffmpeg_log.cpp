#include "mw/streamer/internal/common/ffmpeg_log.h"

#include <cstdarg>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/spdlog.h"

extern "C" {
#include <libavutil/log.h>
}

namespace mw::streamer::internal {
namespace {

struct RepeatedLineState {
  std::mutex mutex;
  std::string last_line;
  spdlog::level::level_enum last_level = spdlog::level::off;
  std::size_t repeat_count = 0;
};

RepeatedLineState& GetRepeatedLineState() {
  static RepeatedLineState state;
  return state;
}

void LogCompleteLine(spdlog::level::level_enum level, std::string_view line) {
  if (line.empty() || level == spdlog::level::off) {
    return;
  }

  RepeatedLineState& state = GetRepeatedLineState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.last_level == level &&
      state.last_line.compare(0, std::string::npos, line.data(), line.size()) ==
          0) {
    ++state.repeat_count;
    return;
  }

  if (state.repeat_count > 0) {
    spdlog::log(state.last_level, "ffmpeg: Last message repeated {} times",
                state.repeat_count);
  }
  state.last_line.assign(line.data(), line.size());
  state.last_level = level;
  state.repeat_count = 0;
  spdlog::log(level, "ffmpeg: {}", line);
}

void ConsumeFormattedText(spdlog::level::level_enum level,
                          std::string_view text) {
  thread_local std::string pending_line;
  pending_line.append(text.data(), text.size());

  std::size_t line_start = 0;
  for (std::size_t index = 0; index < pending_line.size(); ++index) {
    if (pending_line[index] != '\n') {
      continue;
    }

    std::size_t line_end = index;
    if (line_end > line_start && pending_line[line_end - 1] == '\r') {
      --line_end;
    }
    LogCompleteLine(level, std::string_view(pending_line)
                               .substr(line_start, line_end - line_start));
    line_start = index + 1;
  }
  pending_line.erase(0, line_start);
}

void FfmpegLogCallback(void* context, int level, const char* format,
                       va_list arguments) noexcept {
  try {
    const spdlog::level::level_enum spdlog_level = MapFfmpegLogLevel(level);
    if (spdlog_level == spdlog::level::off) {
      return;
    }

    thread_local int print_prefix = 1;
    int probe_print_prefix = print_prefix;
    va_list probe_arguments;
    va_copy(probe_arguments, arguments);
    const int required_size =
        av_log_format_line2(context, level, format, probe_arguments, nullptr, 0,
                            &probe_print_prefix);
    va_end(probe_arguments);
    if (required_size <= 0) {
      return;
    }

    std::vector<char> formatted(static_cast<std::size_t>(required_size) + 1);
    va_list format_arguments;
    va_copy(format_arguments, arguments);
    const int format_result = av_log_format_line2(
        context, level, format, format_arguments, formatted.data(),
        static_cast<int>(formatted.size()), &print_prefix);
    va_end(format_arguments);
    if (format_result < 0) {
      return;
    }

    ConsumeFormattedText(
        spdlog_level,
        std::string_view(formatted.data(),
                         static_cast<std::size_t>(format_result)));
  } catch (...) {
    // Logging must never allow an exception to cross FFmpeg's C callback ABI.
  }
}

}  // namespace

spdlog::level::level_enum MapFfmpegLogLevel(int ffmpeg_level) noexcept {
  if (ffmpeg_level <= AV_LOG_QUIET) {
    return spdlog::level::off;
  }

  const int level_without_color = ffmpeg_level & 0xff;
  if (level_without_color <= AV_LOG_FATAL) {
    return spdlog::level::critical;
  }
  if (level_without_color <= AV_LOG_ERROR) {
    return spdlog::level::err;
  }
  if (level_without_color <= AV_LOG_WARNING) {
    return spdlog::level::warn;
  }
  if (level_without_color <= AV_LOG_INFO) {
    return spdlog::level::info;
  }
  if (level_without_color <= AV_LOG_DEBUG) {
    return spdlog::level::debug;
  }
  return spdlog::level::trace;
}

void InstallFfmpegLogBridge() noexcept {
  static std::once_flag install_once;
  std::call_once(install_once, [] { av_log_set_callback(FfmpegLogCallback); });
}

}  // namespace mw::streamer::internal

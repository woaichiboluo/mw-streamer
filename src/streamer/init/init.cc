#include "mw/streamer/init/init.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <cstdarg>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/Runtime.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/runtime.h"

namespace mw::streamer {
namespace {

log::LogLevel FfmpegLogLevel(int level) noexcept {
  if (level <= AV_LOG_FATAL) return log::LogLevel::kCritical;
  if (level <= AV_LOG_ERROR) return log::LogLevel::kError;
  if (level <= AV_LOG_WARNING) return log::LogLevel::kWarning;
  if (level <= AV_LOG_INFO) return log::LogLevel::kInfo;
  if (level <= AV_LOG_DEBUG) return log::LogLevel::kDebug;
  return log::LogLevel::kTrace;
}

int NativeLogLevel() noexcept {
  for (const int level : {AV_LOG_TRACE, AV_LOG_DEBUG, AV_LOG_INFO,
                          AV_LOG_WARNING, AV_LOG_ERROR, AV_LOG_FATAL}) {
    if (log::ShouldLog("ffmpeg", FfmpegLogLevel(level))) return level;
  }
  return AV_LOG_QUIET;
}

void FfmpegLog(void* instance, int level, const char* format,
               va_list arguments) noexcept {
  if (level < 0) return;
  // FFmpeg can encode a terminal color in the upper bits of the level.
  level &= 0xff;
  const auto mapped = FfmpegLogLevel(level);
  if (!log::ShouldLog("ffmpeg", mapped)) return;
  try {
    char buffer[1024];
    int prefix = 1;
    va_list copy;
    va_copy(copy, arguments);
    const int length = av_log_format_line2(instance, level, format, copy,
                                           buffer, sizeof(buffer), &prefix);
    va_end(copy);
    if (length < 0 || length == std::numeric_limits<int>::max()) return;
    std::vector<char> expanded;
    std::string_view message;
    if (static_cast<std::size_t>(length) < sizeof(buffer)) {
      message = {buffer, static_cast<std::size_t>(length)};
    } else {
      expanded.resize(static_cast<std::size_t>(length) + 1);
      prefix = 1;
      va_copy(copy, arguments);
      const int written = av_log_format_line2(
          instance, level, format, copy, expanded.data(), length + 1, &prefix);
      va_end(copy);
      if (written < 0) return;
      message = {expanded.data(), std::min(static_cast<std::size_t>(written),
                                           expanded.size() - 1)};
    }
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r'))
      message.remove_suffix(1);
    if (!message.empty())
      log::Write("ffmpeg", mapped, __FILE__, __LINE__, message);
  } catch (...) {
    // FFmpeg invokes this from C and codec workers; never unwind through it.
  }
}

}  // namespace

class MwStreamerContext final {
 public:
  ~MwStreamerContext() {
    mediakit::shutdown(zlm_runtime);
    if (ffmpeg_network_initialized) {
      const int error = avformat_network_deinit();
      if (error < 0) {
        MW_LOG_ERROR("streamer", "释放 FFmpeg 网络失败: {}",
                     ffmpeg::AvErrorStr(error));
      }
    }
    if (ffmpeg_logging_initialized) {
      av_log_set_callback(av_log_default_callback);
      av_log_set_level(previous_ffmpeg_log_level);
    }
    logging.reset();
    timer_resolution.reset();
  }

  std::unique_ptr<mw::log::Logging> logging;
  bool ffmpeg_network_initialized = false;
  bool ffmpeg_logging_initialized = false;
  int previous_ffmpeg_log_level = AV_LOG_INFO;
  mediakit::Runtime* zlm_runtime = nullptr;
  std::optional<internal::ScopedTimerResolution> timer_resolution;
};

InitConfig::InitConfig() = default;

MwStreamerContext* Init(const InitConfig& config) {
  internal::EnableUtf8Console();
  auto context = std::make_unique<MwStreamerContext>();
  context->logging = std::make_unique<mw::log::Logging>(config.log);
  context->previous_ffmpeg_log_level = av_log_get_level();
  av_log_set_callback(FfmpegLog);
  context->ffmpeg_logging_initialized = true;
  av_log_set_level(NativeLogLevel());
  internal::IgnoreSigpipe();
  context->timer_resolution.emplace();
  ffmpeg::FfmpegException::throwIfError(avformat_network_init(),
                                        "avformat_network_init");
  context->ffmpeg_network_initialized = true;
  avdevice_register_all();
  mediakit::RuntimeConfig zlm_config;
  zlm_config.event_poller_threads = config.event_poller_threads;
  zlm_config.work_threads = config.work_threads;
  zlm_config.enable_cpu_affinity = config.enable_cpu_affinity;
  zlm_config.tls_certificate_file = config.tls.certificate_file;
  zlm_config.tls_certificate_password = config.tls.certificate_password;
  zlm_config.tls_trusted_certificate_file = config.tls.trusted_certificate_file;
  context->zlm_runtime = mediakit::init(zlm_config);
  return context.release();
}

void Shutdown(MwStreamerContext* context) { delete context; }

}  // namespace mw::streamer

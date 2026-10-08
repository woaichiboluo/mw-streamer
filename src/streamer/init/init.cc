#include "mw/streamer/init/init.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <memory>
#include <optional>

#include "Common/Runtime.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/platform/platform.h"

namespace mw::streamer {
class MwStreamerContext final {
 public:
  ~MwStreamerContext() {
    mediakit::shutdown(zlm_runtime);
    if (ffmpeg_network_initialized) {
      const int error = avformat_network_deinit();
      if (error < 0) {
        MW_LOG_ERROR("streamer", "释放FFmpeg网络失败: {}",
                     ffmpeg::AvErrorStr(error));
      }
    }
    logging.reset();
    timer_resolution.reset();
  }

  std::unique_ptr<mw::log::Logging> logging;
  bool ffmpeg_network_initialized = false;
  mediakit::Runtime* zlm_runtime = nullptr;
  std::optional<internal::ScopedTimerResolution> timer_resolution;
};

InitConfig::InitConfig() { mw_log_default_config(&log); }

MwStreamerContext* Init(const InitConfig& config) {
  auto context = std::make_unique<MwStreamerContext>();
  context->logging = std::make_unique<mw::log::Logging>(config.log);
  context->timer_resolution.emplace();
  ffmpeg::FfmpegException::throwIfError(avformat_network_init(),
                                        "avformat_network_init");
  context->ffmpeg_network_initialized = true;
  mediakit::RuntimeConfig zlm_config;
  zlm_config.event_poller_threads = config.event_poller_threads;
  zlm_config.work_threads = config.work_threads;
  zlm_config.enable_cpu_affinity = config.enable_cpu_affinity;
  context->zlm_runtime = mediakit::init(zlm_config);
  return context.release();
}

void Shutdown(MwStreamerContext* context) { delete context; }

}  // namespace mw::streamer

#include "mw/streamer/init/init.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "Util/util.h"
#include "srt/SrtEpollReactor.h"

namespace mw::streamer {
class MwStreamerContext final {
 public:
  ~MwStreamerContext() {
    if (srt_reactor) {
      srt_reactor->shutdown();
      mediakit::SrtEpollReactor::setInstance(nullptr);
      srt_reactor.reset();
    }
    if (work_pool) {
      work_pool->close();
      toolkit::WorkThreadPool::setInstance(nullptr);
      work_pool.reset();
    }
    if (event_pool) {
      event_pool->close();
      toolkit::EventPollerPool::setInstance(nullptr);
      event_pool.reset();
    }
    if (clock) {
      clock->stop();
      toolkit::setTimestampClock(nullptr);
      clock.reset();
    }
    if (network_initialized) {
      const int error = toolkit::SockUtil::release();
      if (error != 0) {
        MW_LOG_ERROR("streamer", "释放ZLM网络失败: {}", error);
      }
    }
    logging.reset();
  }

  std::unique_ptr<mw::log::Logging> logging;
  bool network_initialized = false;
  std::unique_ptr<toolkit::TimestampClock> clock;
  std::unique_ptr<toolkit::EventPollerPool> event_pool;
  std::unique_ptr<toolkit::WorkThreadPool> work_pool;
  std::unique_ptr<mediakit::SrtEpollReactor> srt_reactor;
};

InitConfig::InitConfig() { mw_log_default_config(&log); }

MwStreamerContext* Init(const InitConfig& config) {
  auto context = std::make_unique<MwStreamerContext>();
  context->logging = std::make_unique<mw::log::Logging>(config.log);
  const int error = toolkit::SockUtil::initialize();
  if (error != 0) {
    throw std::runtime_error("初始化ZLM网络失败: " + std::to_string(error));
  }
  context->network_initialized = true;
  context->clock = std::make_unique<toolkit::TimestampClock>();
  toolkit::setTimestampClock(context->clock.get());

  toolkit::EventPollerPool::setPoolSize(config.event_poller_threads);
  toolkit::EventPollerPool::enableCpuAffinity(config.enable_cpu_affinity);
  toolkit::WorkThreadPool::setPoolSize(config.work_threads);
  toolkit::WorkThreadPool::enableCpuAffinity(config.enable_cpu_affinity);
  context->event_pool = toolkit::EventPollerPool::createPool();
  toolkit::EventPollerPool::setInstance(context->event_pool.get());
  context->work_pool = toolkit::WorkThreadPool::createPool();
  toolkit::WorkThreadPool::setInstance(context->work_pool.get());
  context->srt_reactor = mediakit::SrtEpollReactor::createReactor();
  mediakit::SrtEpollReactor::setInstance(context->srt_reactor.get());
  return context.release();
}

void Shutdown(MwStreamerContext* context) { delete context; }

}  // namespace mw::streamer

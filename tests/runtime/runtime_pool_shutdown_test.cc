#include <memory>
#include <stdexcept>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"
#include "mw/streamer/api.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Runtime关闭会立即释放全部线程池资源") {
  MwLogConfig log_config;
  mw_log_default_config(&log_config);
  MwZlmConfig zlm_config;
  mw_zlm_default_config(&zlm_config);
  zlm_config.event_poller_threads = 1;
  zlm_config.work_threads = 1;
  zlm_config.enable_cpu_affinity = 0;
  REQUIRE(mw_streamer_initialize(&log_config, &zlm_config));

  auto event_pool_owner =
      toolkit::EventPollerPool::Instance().shared_from_this();
  REQUIRE(event_pool_owner.get() == &toolkit::EventPollerPool::Instance());
  event_pool_owner.reset();
  auto work = toolkit::WorkThreadPool::Instance().getFirstPoller();
  auto shared = toolkit::EventPollerPool::Instance().getFirstPoller();
  auto exclusive = toolkit::EventPollerPool::Instance().extractPoller();
  std::weak_ptr<toolkit::EventPoller> weak_work = work;
  std::weak_ptr<toolkit::EventPoller> weak_shared = shared;
  std::weak_ptr<toolkit::EventPoller> weak_exclusive = exclusive;
  work.reset();
  shared.reset();
  exclusive.reset();

  mw_streamer_shutdown();
  mw_streamer_shutdown();

  CHECK_FALSE(mw_streamer_is_initialized());
  CHECK(toolkit::WorkThreadPool::Instance().getExecutorSize() == 0);
  CHECK(toolkit::EventPollerPool::Instance().getExecutorSize() == 0);
  CHECK(weak_work.expired());
  CHECK(weak_shared.expired());
  CHECK(weak_exclusive.expired());
  CHECK_THROWS_AS(toolkit::WorkThreadPool::Instance().getPoller(),
                  std::logic_error);
  CHECK_THROWS_AS(toolkit::EventPollerPool::Instance().getPoller(),
                  std::logic_error);
}

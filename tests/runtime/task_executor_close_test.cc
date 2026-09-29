#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Network/sockutil.h"
#include "Poller/EventPoller.h"
#include "Thread/TaskExecutor.h"
#include "Thread/ThreadPool.h"
#include "Thread/WorkThreadPool.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

class TestPool final : public toolkit::TaskExecutorGetterImp {
 public:
  TestPool() {
    addPoller("close test poller", 1, toolkit::ThreadPool::PRIORITY_NORMAL,
              false, false);
  }

  toolkit::EventPoller::Ptr GetPoller() {
    return std::static_pointer_cast<toolkit::EventPoller>(getFirstExecutor());
  }
};

}  // namespace

TEST_CASE("任务执行器池close可重复调用并立即释放Poller") {
  REQUIRE(toolkit::SockUtil::initialize() == 0);
  std::weak_ptr<toolkit::EventPoller> weak;
  {
    TestPool pool;
    auto poller = pool.GetPoller();
    weak = poller;
    poller.reset();
    pool.close();
    pool.close();
    CHECK(pool.getExecutorSize() == 0);
    CHECK(weak.expired());
  }
  CHECK(toolkit::SockUtil::release() == 0);
}

TEST_CASE("任务执行器池析构会关闭并释放Poller") {
  REQUIRE(toolkit::SockUtil::initialize() == 0);
  std::weak_ptr<toolkit::EventPoller> weak;
  {
    TestPool pool;
    auto poller = pool.GetPoller();
    weak = poller;
  }
  CHECK(weak.expired());
  CHECK(toolkit::SockUtil::release() == 0);
}

TEST_CASE("任务执行器池close支持外部线程并发调用") {
  REQUIRE(toolkit::SockUtil::initialize() == 0);
  TestPool pool;
  auto poller = pool.GetPoller();
  std::weak_ptr<toolkit::EventPoller> weak = poller;
  poller.reset();

  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      pool.close();
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  CHECK(pool.getExecutorSize() == 0);
  CHECK(weak.expired());
  CHECK(toolkit::SockUtil::release() == 0);
}

TEST_CASE("工作线程池可由Runtime式显式关闭且重复关闭安全") {
  REQUIRE(toolkit::SockUtil::initialize() == 0);
  toolkit::WorkThreadPool::setPoolSize(1);
  toolkit::WorkThreadPool::enableCpuAffinity(false);
  auto& pool = toolkit::WorkThreadPool::Instance();
  auto pool_owner = pool.shared_from_this();
  REQUIRE(pool_owner.get() == &pool);
  pool_owner.reset();
  auto poller = pool.getFirstPoller();
  std::weak_ptr<toolkit::EventPoller> weak = poller;
  poller.reset();

  toolkit::WorkThreadPool::releasePool();
  toolkit::WorkThreadPool::releasePool();

  CHECK(pool.getExecutorSize() == 0);
  CHECK(weak.expired());
  CHECK_THROWS_AS(pool.getPoller(), std::logic_error);
  CHECK(toolkit::SockUtil::release() == 0);
}

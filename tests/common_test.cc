#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Util/util.h"
#include "mw/common/barrier.h"
#include "mw/common/blocking_queue.h"
#include "mw/common/thread.h"

#ifdef CHECK
#undef CHECK
#endif

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::common::Barrier;
using mw::streamer::common::BlockingQueue;
using mw::streamer::common::Thread;

}  // namespace

TEST_CASE("BlockingQueue支持move-only数据并在关闭后排空") {
  BlockingQueue<std::unique_ptr<int>> queue;

  CHECK(queue.Push(std::make_unique<int>(7)));
  CHECK(queue.size() == 1);
  queue.Close();
  CHECK(queue.closed());
  CHECK_FALSE(queue.Push(std::make_unique<int>(8)));

  auto value = queue.WaitPop();
  REQUIRE(value.has_value());
  REQUIRE(*value);
  CHECK(**value == 7);
  CHECK_FALSE(queue.WaitPop().has_value());
}

TEST_CASE("BlockingQueue关闭会唤醒等待线程") {
  BlockingQueue<int> queue;
  std::promise<bool> completed;
  auto result = completed.get_future();
  Thread consumer("mw-queue-test",
                  [&]() { completed.set_value(queue.WaitPop().has_value()); });

  std::this_thread::sleep_for(20ms);
  queue.Close();

  REQUIRE(result.wait_for(1s) == std::future_status::ready);
  CHECK_FALSE(result.get());
  consumer.Join();
}

TEST_CASE("BlockingQueue能够清空待处理数据") {
  BlockingQueue<int> queue;
  REQUIRE(queue.Push(1));
  REQUIRE(queue.Push(2));

  CHECK(queue.Clear() == 2);
  CHECK(queue.size() == 0);
  queue.Close();
  CHECK_FALSE(queue.WaitPop().has_value());
}

TEST_CASE("BlockingQueue只对TryPush应用调用方容量限制") {
  BlockingQueue<int> queue;

  CHECK(queue.TryPush(1, 1));
  CHECK_FALSE(queue.TryPush(2, 1));
  CHECK(queue.Push(3));
  CHECK(queue.size() == 2);

  REQUIRE(queue.WaitPop() == 1);
  REQUIRE(queue.WaitPop() == 3);
  queue.Close();
}

TEST_CASE("Thread设置名称并支持显式Join") {
  std::promise<std::string> name;
  auto result = name.get_future();
  Thread thread("mw-common",
                [&]() { name.set_value(toolkit::getThreadName()); });

  CHECK(thread.joinable());
  REQUIRE(result.wait_for(1s) == std::future_status::ready);
  CHECK(result.get() == "mw-common");
  thread.Join();
  CHECK_FALSE(thread.joinable());
  CHECK_NOTHROW(thread.Join());
}

TEST_CASE("Thread拒绝无效入口") {
  CHECK_THROWS_AS(Thread("", []() {}), std::invalid_argument);
  CHECK_THROWS_AS(Thread("mw-common", {}), std::invalid_argument);
}

TEST_CASE("Barrier拒绝零参与者") {
  CHECK_THROWS_AS(Barrier(0), std::invalid_argument);
}

TEST_CASE("Barrier可重复同步并在释放前执行完成函数") {
  constexpr std::size_t kParticipants = 3;
  constexpr std::size_t kRounds = 8;
  Barrier barrier(kParticipants);
  std::atomic<std::size_t> completion_count = 0;
  std::vector<std::future<bool>> workers;

  for (std::size_t participant = 0; participant < kParticipants;
       ++participant) {
    workers.push_back(std::async(std::launch::async, [&]() {
      for (std::size_t round = 0; round < kRounds; ++round) {
        if (!barrier.ArriveAndWait([&]() { completion_count.fetch_add(1); })) {
          return false;
        }
        if (completion_count.load() < round + 1) {
          return false;
        }
      }
      return true;
    }));
  }

  for (auto& worker : workers) {
    CHECK(worker.get());
  }
  CHECK(completion_count.load() == kRounds);
}

TEST_CASE("Barrier取消会唤醒等待者并拒绝后续到达") {
  Barrier barrier(2);
  std::promise<void> started;
  auto started_future = started.get_future();
  auto worker = std::async(std::launch::async, [&]() {
    started.set_value();
    return barrier.ArriveAndWait([]() {});
  });

  started_future.wait();
  std::this_thread::sleep_for(20ms);
  barrier.Cancel();

  REQUIRE(worker.wait_for(1s) == std::future_status::ready);
  CHECK_FALSE(worker.get());
  CHECK_FALSE(barrier.ArriveAndWait([]() {}));
  CHECK_NOTHROW(barrier.Cancel());
}

TEST_CASE("Barrier完成函数异常会取消所有等待者") {
  constexpr std::size_t kParticipants = 3;
  Barrier barrier(kParticipants);
  std::vector<std::future<int>> workers;

  for (std::size_t participant = 0; participant < kParticipants;
       ++participant) {
    workers.push_back(std::async(std::launch::async, [&]() {
      try {
        return barrier.ArriveAndWait(
                   []() { throw std::runtime_error("completion failed"); })
                   ? 1
                   : 0;
      } catch (const std::runtime_error&) {
        return -1;
      }
    }));
  }

  std::size_t exception_count = 0;
  std::size_t cancelled_count = 0;
  for (auto& worker : workers) {
    const int result = worker.get();
    exception_count += result == -1 ? 1 : 0;
    cancelled_count += result == 0 ? 1 : 0;
  }

  CHECK(exception_count == 1);
  CHECK(cancelled_count == kParticipants - 1);
  CHECK_FALSE(barrier.ArriveAndWait([]() {}));
}

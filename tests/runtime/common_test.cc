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
using mw::streamer::Barrier;
using mw::streamer::BlockingQueue;
using mw::streamer::Thread;

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

TEST_CASE("BlockingQueue非阻塞出队保留move-only数据与关闭后的积压") {
  BlockingQueue<std::unique_ptr<int>> queue;
  CHECK_FALSE(queue.TryPop());
  REQUIRE(queue.Push(std::make_unique<int>(7)));
  REQUIRE(queue.Push(std::make_unique<int>(9)));
  auto first = queue.TryPop();
  REQUIRE(first);
  CHECK(**first == 7);
  queue.Close();
  auto last = queue.TryPop();
  REQUIRE(last);
  CHECK(**last == 9);
  CHECK_FALSE(queue.TryPop());
}

TEST_CASE("BlockingQueue观察队头返回独立副本且关闭后仍可观察积压") {
  BlockingQueue<std::string> queue;
  const auto& read_only_queue = queue;
  CHECK_FALSE(read_only_queue.TryPeek());
  REQUIRE(queue.Push("first"));
  REQUIRE(queue.Push("second"));
  auto first = read_only_queue.TryPeek();
  REQUIRE(first);
  CHECK(*first == "first");
  *first = "changed";
  CHECK(read_only_queue.TryPeek() == "first");
  CHECK(queue.size() == 2);

  queue.Close();
  CHECK(read_only_queue.TryPeek() == "first");
  CHECK(queue.TryPop() == "first");
  CHECK(read_only_queue.TryPeek() == "second");
  CHECK(queue.TryPop() == "second");
  CHECK_FALSE(read_only_queue.TryPeek());
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

TEST_CASE("BlockingQueue有界等待由出队或关闭唤醒") {
  BlockingQueue<int> queue;
  REQUIRE(queue.Push(1));
  auto pending = std::async(std::launch::async, [&] {
    return queue.WaitPush(2, 1, [](int value) { return value > 0; });
  });
  CHECK(pending.wait_for(30ms) == std::future_status::timeout);
  SECTION("pop makes room while control messages do not consume quota") {
    REQUIRE(queue.Push(-1));
    CHECK(queue.TryPop() == 1);
    const auto status = pending.wait_for(1s);
    queue.Close();
    REQUIRE(status == std::future_status::ready);
    CHECK(pending.get());
    CHECK(queue.TryPop() == -1);
    CHECK(queue.TryPop() == 2);
  }
  SECTION("close cancels the producer without consuming queued data") {
    queue.Close();
    REQUIRE(pending.wait_for(1s) == std::future_status::ready);
    CHECK_FALSE(pending.get());
    CHECK(queue.TryPop() == 1);
    CHECK_FALSE(queue.TryPop());
  }
}

TEST_CASE("BlockingQueue支持带截止时间的等待") {
  BlockingQueue<int> queue;
  const auto deadline = std::chrono::steady_clock::now() + 20ms;

  CHECK_FALSE(queue.WaitPopUntil(deadline).has_value());
  CHECK_FALSE(queue.closed());
  REQUIRE(queue.Push(7));
  REQUIRE(queue.WaitPopUntil(std::chrono::steady_clock::now() + 1s) == 7);
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

TEST_CASE("BlockingQueue按匹配项限流并在出队或删除后恢复额度") {
  BlockingQueue<int> queue;
  const auto is_data = [](const int& value) { return value > 0; };
  REQUIRE(queue.TryPush(-1, 2, is_data));
  REQUIRE(queue.TryPush(1, 2, is_data));
  REQUIRE(queue.TryPush(-2, 2, is_data));
  REQUIRE(queue.TryPush(2, 2, is_data));
  CHECK_FALSE(queue.TryPush(3, 2, is_data));
  REQUIRE(queue.TryPush(-3, 2, is_data));
  REQUIRE(queue.WaitPop() == -1);
  REQUIRE(queue.WaitPop() == 1);
  REQUIRE(queue.TryPush(3, 2, is_data));
  CHECK(queue.EraseIf([](const int& value) { return value == 2; }) == 1);
  REQUIRE(queue.TryPush(4, 2, is_data));
  CHECK_FALSE(queue.TryPush(5, 2, is_data));
  queue.Close();
  std::vector<int> remaining;
  while (auto value = queue.WaitPop()) {
    remaining.push_back(*value);
  }
  CHECK(remaining == std::vector<int>{-2, -3, 3, 4});
}

TEST_CASE("BlockingQueue零匹配额度仍允许控制项但关闭后一律拒绝") {
  BlockingQueue<int> queue;
  const auto is_data = [](const int& value) { return value > 0; };
  CHECK_FALSE(queue.TryPush(1, 0, is_data));
  REQUIRE(queue.TryPush(-1, 0, is_data));
  queue.Close();
  CHECK_FALSE(queue.TryPush(1, 2, is_data));
  CHECK_FALSE(queue.TryPush(-2, 0, is_data));
  REQUIRE(queue.WaitPop() == -1);
  CHECK_FALSE(queue.WaitPop().has_value());
}

TEST_CASE("BlockingQueue筛选move-only元素并保持剩余顺序") {
  BlockingQueue<std::unique_ptr<int>> queue;
  const auto is_data = [](const std::unique_ptr<int>& value) {
    return *value > 0;
  };
  REQUIRE(queue.TryPush(std::make_unique<int>(1), 2, is_data));
  REQUIRE(queue.TryPush(std::make_unique<int>(-1), 2, is_data));
  REQUIRE(queue.TryPush(std::make_unique<int>(2), 2, is_data));
  REQUIRE(queue.TryPush(std::make_unique<int>(-2), 2, is_data));
  CHECK_FALSE(queue.TryPush(std::make_unique<int>(3), 2, is_data));
  CHECK(queue.EraseIf(is_data) == 2);
  CHECK(queue.EraseIf(is_data) == 0);
  REQUIRE(queue.TryPush(std::make_unique<int>(3), 2, is_data));
  queue.Close();
  std::vector<int> remaining;
  while (auto value = queue.WaitPop()) {
    REQUIRE(*value);
    remaining.push_back(**value);
  }
  CHECK(remaining == std::vector<int>{-1, -2, 3});
}

TEST_CASE("BlockingQueue多生产者同时投递不会超出匹配项总额度") {
  constexpr std::size_t kProducers = 8;
  constexpr std::size_t kAttempts = 32;
  constexpr std::size_t kCapacity = 7;
  BlockingQueue<int> queue;
  Barrier start(kProducers + 1);
  std::atomic<std::size_t> accepted_data = 0;
  std::atomic<std::size_t> accepted_control = 0;
  const auto is_data = [](const int& value) { return value > 0; };
  std::vector<std::unique_ptr<Thread>> producers;
  for (std::size_t i = 0; i < kProducers; ++i) {
    producers.push_back(std::make_unique<Thread>("mw-queue-producer", [&] {
      if (!start.ArriveAndWait([] {})) {
        return;
      }
      for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
        accepted_data.fetch_add(queue.TryPush(1, kCapacity, is_data));
        accepted_control.fetch_add(queue.TryPush(-1, kCapacity, is_data));
      }
    }));
  }
  const bool started = start.ArriveAndWait([] {});
  for (const auto& producer : producers) {
    producer->Join();
  }
  REQUIRE(started);
  CHECK(accepted_data.load() == kCapacity);
  CHECK(accepted_control.load() == kProducers * kAttempts);
  CHECK(queue.size() == kCapacity + kProducers * kAttempts);
  CHECK(queue.EraseIf(is_data) == kCapacity);
  queue.Close();
  std::size_t controls = 0;
  while (auto value = queue.WaitPop()) {
    CHECK(*value == -1);
    ++controls;
  }
  CHECK(controls == kProducers * kAttempts);
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

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Poller/EventPoller.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using Poller = toolkit::EventPoller;

// Each configured pool size runs in a separate test executable because the
// singleton's size cannot be changed after construction.
toolkit::EventPollerPool& Pool() {
  static auto& pool = []() -> toolkit::EventPollerPool& {
    toolkit::EventPollerPool::setPoolSize(MW_POLLER_TEST_POOL_SIZE);
    toolkit::EventPollerPool::enableCpuAffinity(false);
    return toolkit::EventPollerPool::Instance();
  }();
  return pool;
}

class ReleaseGate final {
 public:
  ReleaseGate() : future_(promise_.get_future().share()) {}
  ~ReleaseGate() { promise_.set_value(); }
  std::shared_future<void> future() const { return future_; }

 private:
  std::promise<void> promise_;
  std::shared_future<void> future_;
};

struct ThreadExitSignal {
  explicit ThreadExitSignal(std::shared_ptr<std::promise<void>> signal)
      : signal(std::move(signal)) {}
  ~ThreadExitSignal() { signal->set_value(); }
  std::shared_ptr<std::promise<void>> signal;
};

// A thread-local destructor proves the actual loop thread exited; an expired
// weak_ptr alone only proves the strong reference count reached zero.
void CheckReleaseOnLoop(Poller::Ptr poller) {
  auto exited = std::make_shared<std::promise<void>>();
  auto future = exited->get_future();
  std::weak_ptr<Poller> weak = poller;
  auto* raw = poller.get();
  raw->async(
      [owner = std::move(poller), exited]() mutable {
        thread_local std::unique_ptr<ThreadExitSignal> signal;
        signal = std::make_unique<ThreadExitSignal>(exited);
        owner.reset();
      },
      false);
  REQUIRE(future.wait_for(5s) == std::future_status::ready);
  CHECK(weak.expired());
}

}  // namespace

TEST_CASE("Poller提取保持独占并保留可用的共享池") {
  auto& pool = Pool();
  REQUIRE(pool.getExecutorSize() == MW_POLLER_TEST_POOL_SIZE);
  auto first = pool.extractPoller();
  REQUIRE(first);
  CHECK(pool.getExecutorSize() ==
        (MW_POLLER_TEST_POOL_SIZE > 1 ? MW_POLLER_TEST_POOL_SIZE - 1 : 1));
  // Even after the callback returns and its borrowed reference is gone, a
  // previously published loop may still have raw-pointer tasks pending.
  std::unordered_set<const toolkit::TaskExecutor*> published;
  pool.for_each([&](const toolkit::TaskExecutor::Ptr& shared) {
    published.insert(shared.get());
  });
  const auto shared_count = pool.getExecutorSize();
  auto second = pool.extractPoller();
  REQUIRE(second);
  CHECK(first != second);
  CHECK(published.count(second.get()) == 0);
  CHECK(pool.getExecutorSize() == shared_count);

  toolkit::TaskExecutorGetter& getter = pool;
  toolkit::TaskExecutorGetterImp& implementation = pool;
  const auto is_shared = [&](const toolkit::TaskExecutor::Ptr& executor) {
    return executor && executor != first && executor != second;
  };
  CHECK(is_shared(pool.getPoller()));
  CHECK(is_shared(pool.getPoller(false)));
  CHECK(is_shared(pool.getFirstPoller()));
  CHECK(is_shared(getter.getExecutor()));
  implementation.for_each([&](const toolkit::TaskExecutor::Ptr& executor) {
    CHECK(is_shared(executor));
  });

  auto selected = std::make_shared<std::promise<toolkit::TaskExecutor::Ptr>>();
  auto selected_future = selected->get_future();
  getter.getExecutor([selected](const toolkit::TaskExecutor::Ptr& executor) {
    selected->set_value(executor);
  });
  REQUIRE(selected_future.wait_for(5s) == std::future_status::ready);
  CHECK(is_shared(selected_future.get()));

  auto callback_extract = std::make_shared<std::promise<Poller::Ptr>>();
  auto callback_future = callback_extract->get_future();
  getter.getExecutor(
      [&pool, callback_extract](const toolkit::TaskExecutor::Ptr&) {
        callback_extract->set_value(pool.extractPoller());
      });
  REQUIRE(callback_future.wait_for(5s) == std::future_status::ready);
  auto callback_exclusive = callback_future.get();
  CHECK(published.count(callback_exclusive.get()) == 0);

  auto nested = std::make_shared<std::promise<bool>>();
  auto nested_future = nested->get_future();
  first->async(
      [&pool, nested, raw = first.get()] {
        nested->set_value(pool.getPoller().get() != raw &&
                          pool.getPoller(false).get() != raw);
      },
      false);
  REQUIRE(nested_future.wait_for(5s) == std::future_status::ready);
  CHECK(nested_future.get());

  // Traversal callbacks may acquire or extract another Poller. The pool must
  // release its collection lock before calling arbitrary client code.
  std::vector<Poller::Ptr> reentrant;
  implementation.for_each([&](const toolkit::TaskExecutor::Ptr&) {
    reentrant.push_back(pool.extractPoller());
    CHECK(is_shared(pool.getPoller(false)));
  });
  REQUIRE(reentrant.size() == shared_count);
  CHECK(pool.getExecutorSize() == shared_count);

  auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  {
    ReleaseGate gate;
    first->async(
        [entered, release = gate.future()] {
          entered->set_value();
          release.wait();
        },
        false);
    REQUIRE(entered_future.wait_for(5s) == std::future_status::ready);
    auto responded = std::make_shared<std::promise<void>>();
    auto responded_future = responded->get_future();
    pool.getPoller(false)->async([responded] { responded->set_value(); },
                                 false);
    CHECK(responded_future.wait_for(5s) == std::future_status::ready);
  }

  auto timer = std::make_shared<std::promise<void>>();
  auto timer_future = timer->get_future();
  first->doDelayTask(10, [timer] {
    timer->set_value();
    return uint64_t{0};
  });
  REQUIRE(timer_future.wait_for(5s) == std::future_status::ready);

  std::array<std::vector<Poller::Ptr>, 6> extracted;
  std::vector<std::thread> threads;
  std::atomic<bool> valid{true};
  for (std::size_t index = 0; index < extracted.size(); ++index) {
    threads.emplace_back([&, index] {
      for (int iteration = 0; iteration < 5; ++iteration) {
        auto exclusive = pool.extractPoller();
        if (pool.getPoller(false) == exclusive ||
            getter.getExecutor() == exclusive) {
          valid = false;
        }
        implementation.for_each([&](const toolkit::TaskExecutor::Ptr& shared) {
          if (shared == exclusive) {
            valid = false;
          }
        });
        extracted[index].push_back(std::move(exclusive));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(valid.load());
  std::unordered_set<const toolkit::TaskExecutor*> exclusive_addresses;
  exclusive_addresses.insert(first.get());
  exclusive_addresses.insert(second.get());
  CHECK(exclusive_addresses.insert(callback_exclusive.get()).second);
  for (const auto& exclusive : reentrant) {
    CHECK(exclusive_addresses.insert(exclusive.get()).second);
  }
  for (const auto& group : extracted) {
    for (const auto& exclusive : group) {
      CHECK(exclusive_addresses.insert(exclusive.get()).second);
    }
  }
  implementation.for_each([&](const toolkit::TaskExecutor::Ptr& shared) {
    CHECK(exclusive_addresses.count(shared.get()) == 0);
  });
  CHECK(pool.getExecutorSize() == shared_count);

  // Exercise both a removed pool member (size 3) and newly allocated members.
  CheckReleaseOnLoop(std::move(first));
  CheckReleaseOnLoop(std::move(second));
  CheckReleaseOnLoop(pool.extractPoller());
}

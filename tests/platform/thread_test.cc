#include "mw/streamer/platform/thread.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;
using mw::streamer::internal::Thread;

TEST_CASE("Thread Start returns while its task is still running",
          "[platform][thread][start]") {
  Thread thread;
  std::promise<void> entered;
  auto entry = entered.get_future();
  std::promise<void> release;
  auto allowed = release.get_future();
  std::atomic<bool> finished{false};
  const auto caller = std::this_thread::get_id();
  std::thread::id worker;
  const bool started = thread.Start([&]() noexcept {
    worker = std::this_thread::get_id();
    entered.set_value();
    allowed.wait();
    finished = true;
  });
  // Release before fatal assertions so failures cannot strand a joined task.
  const auto status = entry.wait_for(2s);
  const bool joined = thread.joinable();
  const bool completed_early = finished.load();
  release.set_value();
  thread.Join();
  REQUIRE(started);
  CHECK(status == std::future_status::ready);
  CHECK(worker != caller);
  CHECK(joined);
  CHECK_FALSE(completed_early);
  CHECK(finished.load());
  CHECK_FALSE(thread.joinable());
}

TEST_CASE("Thread rejects empty and repeated starts without replacing work",
          "[platform][thread][validation]") {
  Thread thread;
  CHECK_FALSE(thread.Start({}));
  CHECK_FALSE(thread.joinable());
  std::promise<void> release;
  auto allowed = release.get_future();
  std::atomic<int> calls{0};
  const bool started = thread.Start([&]() noexcept {
    ++calls;
    allowed.wait();
  });
  const bool restarted = thread.Start([&]() noexcept { calls += 100; });
  release.set_value();
  thread.Join();
  REQUIRE(started);
  CHECK_FALSE(restarted);
  CHECK(calls.load() == 1);
}

TEST_CASE("Thread can restart after Join and Join is idempotent",
          "[platform][thread][restart]") {
  Thread thread;
  thread.Join();
  int calls = 0;
  for (int iteration = 0; iteration < 100; ++iteration) {
    REQUIRE(thread.Start([&]() noexcept { ++calls; }));
    thread.Join();
    thread.Join();
    CHECK_FALSE(thread.joinable());
  }
  CHECK(calls == 100);
}

TEST_CASE("Thread destructor joins its task", "[platform][thread][lifetime]") {
  std::promise<void> release;
  auto allowed = release.get_future();
  bool finished = false;
  {
    Thread thread;
    const bool started = thread.Start([&]() noexcept {
      allowed.wait();
      finished = true;
    });
    release.set_value();
    REQUIRE(started);
  }
  CHECK(finished);
}

}  // namespace

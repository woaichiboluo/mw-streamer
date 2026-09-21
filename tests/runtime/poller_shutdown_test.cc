#include <atomic>
#include <future>
#include <iostream>
#include <memory>

#include "Poller/EventPoller.h"
#include "Thread/WorkThreadPool.h"

int main() {
  if (toolkit::EventPollerPool::getInstanceIfCreated() ||
      toolkit::WorkThreadPool::getInstanceIfCreated()) {
    return 1;
  }
  toolkit::EventPollerPool::setPoolSize(1);
  toolkit::WorkThreadPool::setPoolSize(1);
  auto& events = toolkit::EventPollerPool::Instance();
  auto& work = toolkit::WorkThreadPool::Instance();
  auto exclusive = events.extractPoller();
  std::atomic<int> completed{0};

  // A pending producer keeps a private input loop alive and posts teardown
  // which itself posts more teardown. Shutdown must finish both on its owner.
  work.getPoller()->async([exclusive, &completed] {
    exclusive->async([exclusive, &completed] {
      exclusive->async([&completed] { ++completed; }, false);
      ++completed;
    }, false);
  }, false);
  exclusive.reset();

  // Releasing the client's last reference on the loop must not destroy it:
  // the pool owns every exclusive loop until terminal shutdown.
  auto self_released = events.extractPoller();
  std::weak_ptr<toolkit::EventPoller> weak = self_released;
  std::promise<void> released;
  auto release_done = released.get_future();
  auto* raw = self_released.get();
  raw->async([owner = std::move(self_released), &released]() mutable {
    owner.reset();
    released.set_value();
  }, false);
  release_done.wait();
  if (weak.expired()) {
    return 2;
  }

  // A far-future callback retaining its own poller must be cancelled, not
  // left in a reference cycle after the pool releases its exclusive set.
  auto delayed = events.extractPoller();
  std::weak_ptr<toolkit::EventPoller> delayed_weak = delayed;
  delayed->doDelayTask(60000, [delayed] { return uint64_t{0}; });
  delayed->sync([] {});
  delayed.reset();

  work.shutdown();
  events.shutdown();
  work.shutdown();
  events.shutdown();
  if (!weak.expired() || !delayed_weak.expired() || completed != 2 || work.getExecutorSize() != 0 ||
      events.getExecutorSize() != 0) {
    return 3;
  }
  try {
    events.extractPoller();
    return 4;
  } catch (const std::logic_error&) {
  }
  std::cout << "poller shutdown completed\n";
  return 0;
}

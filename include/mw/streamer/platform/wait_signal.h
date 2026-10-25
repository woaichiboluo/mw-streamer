#ifndef MW_STREAMER_PLATFORM_WAIT_SIGNAL_H_
#define MW_STREAMER_PLATFORM_WAIT_SIGNAL_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mw::streamer::internal {

// Combines queue readiness and local scheduling events without executing user
// code. Record a revision before inspecting queues/state, then wait on it; a
// notification between inspection and waiting is retained. Owners must wake
// and join waiters before destruction. This signal does not close any queue.
class WaitSignal final {
 public:
  using Revision = std::uint64_t;

  Revision GetRevision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
  }

  void Notify() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++revision_;
    }
    changed_.notify_all();
  }

  void Wait(Revision observed) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this, observed] { return revision_ != observed; });
  }

  // The consumer owns this scheduling deadline; queues expose no timed Pop.
  // Returns true for a revision change, false when the deadline expires.
  bool WaitUntil(Revision observed,
                 std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_until(
        lock, deadline, [this, observed] { return revision_ != observed; });
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Revision revision_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PLATFORM_WAIT_SIGNAL_H_

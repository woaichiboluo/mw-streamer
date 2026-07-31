#ifndef MW_STREAMER_INCLUDE_MW_COMMON_BARRIER_H_
#define MW_STREAMER_INCLUDE_MW_COMMON_BARRIER_H_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mw::streamer::common {

class Barrier final {
 public:
  explicit Barrier(std::size_t participants)
      : participants_(participants), remaining_(participants) {
    if (participants == 0) {
      throw std::invalid_argument("Barrier参与者数量必须大于0");
    }
  }

  Barrier(const Barrier&) = delete;
  Barrier& operator=(const Barrier&) = delete;

  // The last participant runs completion before releasing the others. Returns
  // false after cancellation. If completion throws, that exception is rethrown
  // to the last participant and all other participants return false.
  template <typename Completion>
  bool ArriveAndWait(Completion&& completion) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cancelled_) {
      return false;
    }

    const std::size_t generation = generation_;
    --remaining_;
    if (remaining_ != 0) {
      condition_.wait(lock, [this, generation]() {
        return cancelled_ || generation_ != generation;
      });
      return !cancelled_;
    }

    try {
      std::invoke(std::forward<Completion>(completion));
    } catch (...) {
      cancelled_ = true;
      lock.unlock();
      condition_.notify_all();
      throw;
    }

    remaining_ = participants_;
    ++generation_;
    lock.unlock();
    condition_.notify_all();
    return true;
  }

  // Cancellation is permanent and wakes all waiting participants.
  void Cancel() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
    }
    condition_.notify_all();
  }

 private:
  const std::size_t participants_;
  std::size_t remaining_;
  std::size_t generation_ = 0;
  bool cancelled_ = false;
  std::mutex mutex_;
  std::condition_variable condition_;
};

}  // namespace mw::streamer::common

#endif  // MW_STREAMER_INCLUDE_MW_COMMON_BARRIER_H_

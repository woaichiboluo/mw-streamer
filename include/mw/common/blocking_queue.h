#ifndef MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_
#define MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace mw::streamer {

template <typename T>
class BlockingQueue final {
 public:
  BlockingQueue() = default;
  ~BlockingQueue() = default;

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  // Returns false after Close. Producers never wait for consumers.
  bool Push(T value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    condition_.notify_one();
    return true;
  }

  // Applies a caller-provided limit without changing the queue's unbounded
  // Push semantics. This lets data messages be bounded while control messages
  // continue to use Push.
  bool TryPush(T value, std::size_t max_size) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || queue_.size() >= max_size) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    condition_.notify_one();
    return true;
  }

  // Limits only elements matching predicate; nonmatching values use no quota.
  // Counting and insertion are atomic with respect to other queue operations.
  // Predicate receives const T& under the queue lock and must not re-enter it.
  template <typename Predicate>
  bool TryPush(T value, std::size_t max_matching, Predicate predicate) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      if (predicate(std::as_const(value)) &&
          static_cast<std::size_t>(std::count_if(queue_.cbegin(), queue_.cend(),
                                                 predicate)) >= max_matching) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    condition_.notify_one();
    return true;
  }

  // Waits for space instead of dropping the value. Control messages can still
  // use Push. Close wakes blocked producers and causes them to return false.
  // max_matching must be positive; predicate has the same contract as TryPush.
  template <typename Predicate>
  bool WaitPush(T value, std::size_t max_matching, Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex_);
    space_available_.wait(lock, [&]() {
      return closed_ || !predicate(std::as_const(value)) ||
             static_cast<std::size_t>(std::count_if(
                 queue_.cbegin(), queue_.cend(), predicate)) < max_matching;
    });
    if (closed_) {
      return false;
    }
    queue_.push_back(std::move(value));
    lock.unlock();
    condition_.notify_one();
    return true;
  }

  // Blocks until an item is available or the queue is closed. Close preserves
  // queued items; nullopt is returned only after the closed queue is empty.
  std::optional<T> WaitPop() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    space_available_.notify_all();
    return value;
  }

  // Never waits. Close preserves queued items for both pop operations.
  std::optional<T> TryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    space_available_.notify_all();
    return value;
  }

  // Copies the front item without removing it, including after Close. Returns
  // nullopt when empty. Only callers of this method require T to be copyable.
  std::optional<T> TryPeek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    return queue_.front();
  }

  // Returns nullopt when the deadline expires or after a closed queue becomes
  // empty. Call closed() to distinguish those two outcomes.
  template <typename Clock, typename Duration>
  std::optional<T> WaitPopUntil(
      const std::chrono::time_point<Clock, Duration>& deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_until(
            lock, deadline, [this]() { return closed_ || !queue_.empty(); })) {
      return std::nullopt;
    }
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    space_available_.notify_all();
    return value;
  }

  std::size_t Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto size = queue_.size();
    queue_.clear();
    space_available_.notify_all();
    return size;
  }

  // Removes matching elements and returns their count, preserving the order of
  // retained elements. Predicate receives const T& under the queue lock and
  // must not re-enter it.
  template <typename Predicate>
  std::size_t EraseIf(Predicate predicate) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto previous_size = queue_.size();
    queue_.erase(
        std::remove_if(queue_.begin(), queue_.end(),
                       [&](const T& value) { return predicate(value); }),
        queue_.end());
    space_available_.notify_all();
    return previous_size - queue_.size();
  }

  // Close is idempotent and wakes every waiting consumer and producer.
  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    condition_.notify_all();
    space_available_.notify_all();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable space_available_;
  std::deque<T> queue_;
  bool closed_ = false;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_

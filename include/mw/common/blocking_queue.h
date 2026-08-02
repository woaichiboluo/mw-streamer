#ifndef MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_
#define MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace mw::streamer::common {

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
    return value;
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
    return value;
  }

  std::size_t Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto size = queue_.size();
    queue_.clear();
    return size;
  }

  // Close is idempotent and wakes every waiting consumer.
  void Close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    condition_.notify_all();
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
  std::deque<T> queue_;
  bool closed_ = false;
};

}  // namespace mw::streamer::common

#endif  // MW_STREAMER_INCLUDE_MW_COMMON_BLOCKING_QUEUE_H_

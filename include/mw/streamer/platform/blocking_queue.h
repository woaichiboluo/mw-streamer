#ifndef MW_STREAMER_PLATFORM_BLOCKING_QUEUE_H_
#define MW_STREAMER_PLATFORM_BLOCKING_QUEUE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "mw/streamer/platform/wait_signal.h"
#include "mw/streamer/platform/write_signal.h"

namespace mw::streamer::internal {

enum class BlockingQueueResult { kSuccess, kTimeout, kEmpty, kClosed };

// Fixed-capacity FIFO supporting multiple producers and consumers. Close wakes
// waiters and rejects new items; consumers can still drain accepted items.
// Owners must Close and join all users before destroying the queue. Size is a
// snapshot, not a reservation. Element operations must not reenter this queue.
template <typename T>
class BlockingQueue {
 public:
  explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("BlockingQueue capacity must be positive");
    }
  }

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  BlockingQueueResult Push(const T& value) {
    return PushImpl(value, std::nullopt);
  }

  BlockingQueueResult Push(T&& value) {
    return PushImpl(std::move(value), std::nullopt);
  }

  // Timeout covers waiting for capacity. kTimeout/kClosed do not move value.
  // Element construction exceptions propagate with T's exception guarantees.
  BlockingQueueResult Push(const T& value,
                           std::chrono::steady_clock::duration timeout) {
    return PushImpl(value, timeout);
  }

  BlockingQueueResult Push(T&& value,
                           std::chrono::steady_clock::duration timeout) {
    return PushImpl(std::move(value), timeout);
  }

  BlockingQueueResult Pop(T& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    return PopImpl(value, lock);
  }

  BlockingQueueResult TryPop(T& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    return PopImpl(value, lock);
  }

  // Optional readiness signal for a consumer selecting among several queues.
  // Registration/replacement is thread-safe; no user callbacks are invoked.
  void SetReadSignal(std::weak_ptr<WaitSignal> signal) {
    std::shared_ptr<WaitSignal> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      read_signal_ = std::move(signal);
      if (closed_ || !queue_.empty()) ready = read_signal_.lock();
    }
    if (ready) ready->Notify();
  }

  // Wakes an event-loop producer on capacity release or closure. Registration
  // replays existing space, so binding after a failed Push cannot lose it.
  void SetWriteSignal(std::weak_ptr<WriteSignal> signal) {
    std::shared_ptr<WriteSignal> writable;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      write_signal_ = std::move(signal);
      if (closed_ || queue_.size() < capacity_) writable = write_signal_.lock();
    }
    if (writable) writable->Notify();
  }

  void Close() {
    std::shared_ptr<WaitSignal> ready;
    std::shared_ptr<WriteSignal> writable;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      ready = read_signal_.lock();
      writable = write_signal_.lock();
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    if (ready) ready->Notify();
    if (writable) writable->Notify();
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  std::size_t Capacity() const { return capacity_; }

 private:
  template <typename U>
  BlockingQueueResult PushImpl(
      U&& value, std::optional<std::chrono::steady_clock::duration> timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [this] { return closed_ || queue_.size() < capacity_; };
    if (timeout) {
      if (!not_full_.wait_for(lock, *timeout, ready)) {
        return BlockingQueueResult::kTimeout;
      }
    } else {
      not_full_.wait(lock, ready);
    }
    if (closed_) {
      return BlockingQueueResult::kClosed;
    }
    try {
      queue_.emplace_back(std::forward<U>(value));
    } catch (...) {
      // A failed insertion must not consume the only capacity notification.
      lock.unlock();
      not_full_.notify_one();
      throw;
    }
    auto signal = read_signal_.lock();
    lock.unlock();
    not_empty_.notify_one();
    if (signal) signal->Notify();
    return BlockingQueueResult::kSuccess;
  }

  BlockingQueueResult PopImpl(T& value, std::unique_lock<std::mutex>& lock) {
    if (queue_.empty()) {
      return closed_ ? BlockingQueueResult::kClosed
                     : BlockingQueueResult::kEmpty;
    }
    try {
      value = std::move(queue_.front());
    } catch (...) {
      // The item stays queued, with the guarantees of T's move assignment.
      lock.unlock();
      not_empty_.notify_one();
      throw;
    }
    queue_.pop_front();
    auto signal = write_signal_.lock();
    lock.unlock();
    not_full_.notify_one();
    if (signal) signal->Notify();
    return BlockingQueueResult::kSuccess;
  }

  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<T> queue_;
  std::weak_ptr<WaitSignal> read_signal_;
  std::weak_ptr<WriteSignal> write_signal_;
  bool closed_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PLATFORM_BLOCKING_QUEUE_H_

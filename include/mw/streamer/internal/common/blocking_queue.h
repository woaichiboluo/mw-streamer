#ifndef MW_STREAMER_INTERNAL_COMMON_BLOCKING_QUEUE_H_
#define MW_STREAMER_INTERNAL_COMMON_BLOCKING_QUEUE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {

template <typename T>
class BlockingQueue final {
 public:
  explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      ThrowError(StatusCode::kInvalidArgument,
                 "blocking queue capacity must be positive");
    }
  }

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;
  BlockingQueue(BlockingQueue&&) = delete;
  BlockingQueue& operator=(BlockingQueue&&) = delete;

  [[nodiscard]] bool Push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock,
                   [this] { return closed_ || values_.size() < capacity_; });
    if (closed_) {
      return false;
    }
    values_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  [[nodiscard]] bool PushDroppingOldest(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    if (values_.size() == capacity_) {
      values_.pop_front();
    }
    values_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  [[nodiscard]] bool TryPush(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || values_.size() == capacity_) {
      return false;
    }
    values_.push_back(std::move(value));
    not_empty_.notify_one();
    return true;
  }

  [[nodiscard]] std::optional<T> Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !values_.empty(); });
    if (values_.empty()) {
      return std::nullopt;
    }
    T value = std::move(values_.front());
    values_.pop_front();
    not_full_.notify_one();
    return value;
  }

  [[nodiscard]] std::optional<T> TryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (values_.empty()) {
      return std::nullopt;
    }
    T value = std::move(values_.front());
    values_.pop_front();
    not_full_.notify_one();
    return value;
  }

  template <typename Rep, typename Period>
  [[nodiscard]] std::optional<T> PopFor(
      const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait_for(lock, timeout,
                        [this] { return closed_ || !values_.empty(); });
    if (values_.empty()) {
      return std::nullopt;
    }
    T value = std::move(values_.front());
    values_.pop_front();
    not_full_.notify_one();
    return value;
  }

  void Close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  void Cancel() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    values_.clear();
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  void Clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    not_full_.notify_all();
  }

  [[nodiscard]] bool is_closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> values_;
  bool closed_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_BLOCKING_QUEUE_H_

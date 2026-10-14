#ifndef MW_STREAMER_PLATFORM_THREAD_H_
#define MW_STREAMER_PLATFORM_THREAD_H_

#include <functional>
#include <thread>

#include "mw/export.h"

namespace mw::streamer::internal {

// Start/Join/destruction must be externally serialized and cannot be called
// from this thread's task. The owner must arrange for the task to return before
// Join or destruction; Thread does not request cancellation or detach.
class MW_STREAMER_API Thread final {
 public:
  Thread() = default;
  ~Thread();
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

  // Waits until the new thread has entered its execution wrapper, not until
  // task initialization or processing completes. The task must not throw.
  // Returns false and logs if task is empty, Join is required before reuse,
  // or thread creation fails. Allocation failure is not recovered.
  [[nodiscard]] bool Start(std::function<void()> task) noexcept;
  // Waits for completion, then permits another Start. Safe before Start and
  // after Join. Does not interrupt the task.
  void Join() noexcept;
  // True until Join, even if the task has already returned.
  bool joinable() const noexcept;

 private:
  std::thread thread_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PLATFORM_THREAD_H_

#ifndef MW_STREAMER_PLATFORM_PLATFORM_H_
#define MW_STREAMER_PLATFORM_PLATFORM_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "mw/export.h"

namespace mw::streamer::internal {

MW_STREAMER_API bool HasEnvironmentVariable(const char* name) noexcept;

// Owned by Init/Shutdown; destroy after all media workers have stopped.
class ScopedTimerResolution final {
 public:
  ScopedTimerResolution();
  ~ScopedTimerResolution();
  ScopedTimerResolution(const ScopedTimerResolution&) = delete;
  ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;
};

// Construct and destroy on the audio output thread. Registration is best
// effort.
class ScopedAudioScheduling final {
 public:
  ScopedAudioScheduling() noexcept;
  ~ScopedAudioScheduling();
  ScopedAudioScheduling(const ScopedAudioScheduling&) = delete;
  ScopedAudioScheduling& operator=(const ScopedAudioScheduling&) = delete;

 private:
  void* task_ = nullptr;
};

// Returns false when cancelled, true when the deadline is reached or passed.
// Change stop under wait_mutex, then notify wake. Precise waiting releases the
// mutex before its final spin so another output worker and Stop can progress.
bool WaitUntil(std::condition_variable& wake, std::mutex& wait_mutex,
               const std::atomic<bool>& stop,
               std::chrono::steady_clock::time_point deadline,
               bool precise) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PLATFORM_PLATFORM_H_

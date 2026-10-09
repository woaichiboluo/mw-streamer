#include <signal.h>

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include "mw/streamer/platform/platform.h"

namespace mw::streamer::internal {

bool HasEnvironmentVariable(const char* name) noexcept {
  return std::getenv(name) != nullptr;
}

void IgnoreSigpipe() {
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPIPE, &action, nullptr) != 0) {
    throw std::system_error(errno, std::generic_category(), "忽略SIGPIPE失败");
  }
}

// Retain the existing non-Windows scheduling behavior.
ScopedTimerResolution::ScopedTimerResolution() = default;
ScopedTimerResolution::~ScopedTimerResolution() = default;

ScopedAudioScheduling::ScopedAudioScheduling() noexcept = default;
ScopedAudioScheduling::~ScopedAudioScheduling() = default;

bool WaitUntil(std::condition_variable& wake, std::mutex& wait_mutex,
               const std::atomic<bool>& stop,
               std::chrono::steady_clock::time_point deadline,
               bool precise) noexcept {
  (void)precise;
  std::unique_lock<std::mutex> lock(wait_mutex);
  if (wake.wait_until(lock, deadline, [&stop] { return stop.load(); }))
    return false;
  lock.unlock();
  return !stop.load();
}

}  // namespace mw::streamer::internal

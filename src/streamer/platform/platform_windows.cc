#include <Windows.h>
#include <avrt.h>
#include <mmsystem.h>

#include <cstdlib>
#include <stdexcept>

#include "mw/streamer/platform/platform.h"

namespace mw::streamer::internal {

bool HasEnvironmentVariable(const char* name) noexcept {
  std::size_t size = 0;
  getenv_s(&size, nullptr, 0, name);
  return size != 0;
}

void IgnoreSigpipe() {}

ScopedTimerResolution::ScopedTimerResolution() {
  if (timeBeginPeriod(1) != TIMERR_NOERROR) {
    throw std::runtime_error("无法申请Windows 1ms定时精度");
  }
}

ScopedTimerResolution::~ScopedTimerResolution() { timeEndPeriod(1); }

ScopedAudioScheduling::ScopedAudioScheduling() noexcept {
  DWORD task_index = 0;
  task_ = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
}

ScopedAudioScheduling::~ScopedAudioScheduling() {
  if (task_) AvRevertMmThreadCharacteristics(task_);
}

bool WaitUntil(std::condition_variable& wake, std::mutex& wait_mutex,
               const std::atomic<bool>& stop,
               std::chrono::steady_clock::time_point deadline,
               bool precise) noexcept {
  std::unique_lock<std::mutex> lock(wait_mutex);
  const auto wake_at =
      precise ? deadline - std::chrono::milliseconds(1) : deadline;
  if (wake.wait_until(lock, wake_at, [&stop] { return stop.load(); }))
    return false;
  lock.unlock();
  if (precise) {
    while (std::chrono::steady_clock::now() < deadline) {
      if (stop.load()) return false;
      YieldProcessor();
    }
  }
  return !stop.load();
}

}  // namespace mw::streamer::internal

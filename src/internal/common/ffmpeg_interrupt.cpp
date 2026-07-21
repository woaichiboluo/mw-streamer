#include "mw/streamer/internal/common/ffmpeg_interrupt.h"

#include <chrono>
#include <cstdint>

namespace mw::streamer::internal {
namespace {

std::int64_t SteadyNanoseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

AVIOInterruptCB FfmpegInterrupt::callback() noexcept {
  return AVIOInterruptCB{&FfmpegInterrupt::Check, this};
}

void FfmpegInterrupt::Arm(std::chrono::milliseconds timeout) noexcept {
  timed_out_.store(false, std::memory_order_release);
  if (timeout.count() <= 0) {
    deadline_nanoseconds_.store(0, std::memory_order_release);
    return;
  }
  const std::int64_t timeout_nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
  deadline_nanoseconds_.store(SteadyNanoseconds() + timeout_nanoseconds,
                              std::memory_order_release);
}

void FfmpegInterrupt::Disarm() noexcept {
  deadline_nanoseconds_.store(0, std::memory_order_release);
}

void FfmpegInterrupt::Cancel() noexcept {
  cancelled_.store(true, std::memory_order_release);
}

bool FfmpegInterrupt::is_cancelled() const noexcept {
  return cancelled_.load(std::memory_order_acquire);
}

bool FfmpegInterrupt::timed_out() const noexcept {
  return timed_out_.load(std::memory_order_acquire);
}

int FfmpegInterrupt::Check(void* opaque) noexcept {
  if (opaque == nullptr) {
    return 0;
  }
  return static_cast<FfmpegInterrupt*>(opaque)->Check();
}

int FfmpegInterrupt::Check() noexcept {
  if (cancelled_.load(std::memory_order_acquire)) {
    return 1;
  }
  const std::int64_t deadline =
      deadline_nanoseconds_.load(std::memory_order_acquire);
  if (deadline != 0 && SteadyNanoseconds() >= deadline) {
    timed_out_.store(true, std::memory_order_release);
    return 1;
  }
  return 0;
}

}  // namespace mw::streamer::internal

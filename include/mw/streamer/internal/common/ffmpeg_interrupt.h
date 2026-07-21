#ifndef MW_STREAMER_INTERNAL_COMMON_FFMPEG_INTERRUPT_H_
#define MW_STREAMER_INTERNAL_COMMON_FFMPEG_INTERRUPT_H_

#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libavformat/avio.h>
}

namespace mw::streamer::internal {

class FfmpegInterrupt final {
 public:
  [[nodiscard]] AVIOInterruptCB callback() noexcept;

  void Arm(std::chrono::milliseconds timeout) noexcept;
  void Disarm() noexcept;
  void Cancel() noexcept;

  [[nodiscard]] bool is_cancelled() const noexcept;
  [[nodiscard]] bool timed_out() const noexcept;

 private:
  static int Check(void* opaque) noexcept;
  [[nodiscard]] int Check() noexcept;

  std::atomic<bool> cancelled_{false};
  std::atomic<bool> timed_out_{false};
  std::atomic<std::int64_t> deadline_nanoseconds_{0};
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_FFMPEG_INTERRUPT_H_

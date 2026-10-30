#ifndef MW_SRC_STREAMER_PERFORMANCE_LATENCY_H_
#define MW_SRC_STREAMER_PERFORMANCE_LATENCY_H_

#include <chrono>
#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/buffer.h>
}

#include "mw/streamer/performance/config.h"

namespace mw::streamer::internal {

// One serial owner per stage. Audio/video workers use separate instances.
// Begin/End measure elapsed time within the caller's stage boundaries, not CPU
// time. Attach/Complete also include codec/filter buffering between those
// boundaries. Missing opaque propagation is an unavailable sample.
class StageLatency final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct Samples {
    std::uint64_t valid = 0;
    std::uint64_t unavailable = 0;
    std::int64_t total_ns = 0;
    std::int64_t max_ns = 0;
  };

  StageLatency() noexcept = default;
  ~StageLatency();
  StageLatency(const StageLatency&) = delete;
  StageLatency& operator=(const StageLatency&) = delete;

  // Captures the runtime's fixed stage mode and its built-in log names.
  void Start(PerformanceStage stage, const void* instance,
             int stream_index = -1) noexcept;
  bool enabled() const noexcept { return active_; }
  TimePoint Begin() const noexcept;
  void End(TimePoint started) noexcept;

  // Operate on private AVPacket/AVFrame headers. The immutable probe owns the
  // original opaque_ref; completion restores a reference to that exact buffer.
  // Copies/repeated outputs can complete independently without mutating it.
  void Attach(AVBufferRef*& opaque, TimePoint started);
  void Complete(AVBufferRef*& opaque);
  void Complete(AVBufferRef*& opaque, TimePoint finished);

  void Observe(std::optional<std::int64_t> nanoseconds) noexcept;
  const Samples& samples() const noexcept { return total_; }
  void Finish() noexcept;

 private:
  void Report(bool final) noexcept;

  Samples total_;
  Samples interval_;
  TimePoint started_{};
  TimePoint reported_{};
  const char* module_ = nullptr;
  const char* stage_ = nullptr;
  const void* instance_ = nullptr;
  std::uint64_t token_ = 0;
  int stream_index_ = -1;
  bool active_ = false;
  bool trace_ = false;
};

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_PERFORMANCE_LATENCY_H_

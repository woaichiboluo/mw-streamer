#ifndef MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STAGE_RECORDER_H_
#define MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STAGE_RECORDER_H_

#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" {
#include <hdr/hdr_interval_recorder.h>
}

#include "mw/performance/snapshot.h"

namespace mw::streamer::performance::internal {

struct StageInterval {
  std::uint64_t units = 0;
  LatencySnapshot latency;
};

class alignas(64) StageRecorder final {
 public:
  StageRecorder();
  ~StageRecorder();

  StageRecorder(const StageRecorder&) = delete;
  StageRecorder& operator=(const StageRecorder&) = delete;

  void Record(std::uint64_t units, std::chrono::nanoseconds latency) noexcept;
  StageInterval Collect() noexcept;

 private:
  std::atomic<std::uint64_t> completed_units_{0};
  hdr_interval_recorder latency_{};
};

class TrackRecorder final {
 public:
  StageRecorder& decode() noexcept { return decode_; }
  StageRecorder& process() noexcept { return process_; }
  StageRecorder& encode() noexcept { return encode_; }

  void RecordDroppedPackets(std::uint64_t count) noexcept {
    dropped_packets_.fetch_add(count, std::memory_order_relaxed);
  }

  std::uint64_t CollectDroppedPackets() noexcept {
    return dropped_packets_.exchange(0, std::memory_order_relaxed);
  }

  void Reset() noexcept {
    static_cast<void>(decode_.Collect());
    static_cast<void>(process_.Collect());
    static_cast<void>(encode_.Collect());
    static_cast<void>(CollectDroppedPackets());
  }

 private:
  StageRecorder decode_;
  StageRecorder process_;
  StageRecorder encode_;
  std::atomic<std::uint64_t> dropped_packets_{0};
};

}  // namespace mw::streamer::performance::internal

#endif  // MW_STREAMER_INCLUDE_MW_PERFORMANCE_INTERNAL_STAGE_RECORDER_H_

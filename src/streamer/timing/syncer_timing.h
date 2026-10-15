#ifndef MW_STREAMER_TIMING_SYNCER_TIMING_H_
#define MW_STREAMER_TIMING_SYNCER_TIMING_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "mw/streamer/timing/syncer.h"

namespace mw::streamer::internal {

// Pure scheduling state, driven only by Syncer's worker. Microsecond clock
// values are elapsed steady-clock time since Start, not wall-clock epochs.
struct SyncerStep {
  bool consumed = false;
  std::optional<ffmpeg::Frame> frame;
  std::int64_t wake_us = 0;
};

class SyncerTiming final {
 public:
  explicit SyncerTiming(const SyncerConfig& config,
                        const void* log_context = nullptr);
  ~SyncerTiming();
  SyncerTiming(const SyncerTiming&) = delete;
  SyncerTiming& operator=(const SyncerTiming&) = delete;
  // A head stays in the owner's queue until consumed. May emit one filler
  // without consuming the head. No input is represented by nullptr, not EOF.
  SyncerStep Step(bool video, const AvDecoderReadResult* head,
                  std::int64_t now_us);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_TIMING_SYNCER_TIMING_H_

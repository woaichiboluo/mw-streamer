#ifndef MW_STREAMER_SYNCER_SYNCER_TIMING_H_
#define MW_STREAMER_SYNCER_SYNCER_TIMING_H_

#include <cstdint>
#include <memory>

#include "mw/streamer/syncer/syncer.h"

namespace mw::streamer::internal {

// Internal passive continuity state, serialized by its owner.
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

#endif  // MW_STREAMER_SYNCER_SYNCER_TIMING_H_

#ifndef MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_REALTIME_FRAME_SCHEDULER_H_
#define MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_REALTIME_FRAME_SCHEDULER_H_

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>

#include "mw/media/stream_event.h"
#include "mw/synchronizer/config.h"

namespace mw::streamer::internal {

// Used exclusively by SynchronizerSink's worker. Owns bounded per-track frame
// caches, prototypes and a continuous output clock; it owns no thread itself.
class RealtimeFrameScheduler final {
 public:
  using Clock = std::chrono::steady_clock;
  struct OutputFrame {
    bool audio;
    Frame frame;
  };

  explicit RealtimeFrameScheduler(SynchronizerSinkConfig config);
  ~RealtimeFrameScheduler();
  RealtimeFrameScheduler(const RealtimeFrameScheduler&) = delete;
  RealtimeFrameScheduler& operator=(const RealtimeFrameScheduler&) = delete;

  // Copies borrowed hardware context ownership. Replacement metadata must be
  // compatible; input generation validation belongs to the containing sink.
  void Configure(const FrameStreamsReady& streams);
  void Push(const FrameReady& frame, bool audio, Clock::time_point now);
  // Replaces the source mapping while preserving output clocks and prototypes.
  void Reset();
  // Ends synthesis once retained media drains. Missing initial prototypes fail.
  void Finish();
  std::optional<OutputFrame> TakeReady(Clock::time_point now);
  std::optional<Clock::time_point> deadline() const;
  bool finished() const;
  bool standby() const;
  std::size_t queue_depth() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_INTERNAL_REALTIME_FRAME_SCHEDULER_H_

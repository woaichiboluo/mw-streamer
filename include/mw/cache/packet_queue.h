#ifndef MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_
#define MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "mw/sink/sink.h"

namespace mw::streamer {

// Owns its scheduling thread and separate audio/video caches sharing one clock.
// Input notifications are copied and processed in submission order. Zero cache
// duration forwards immediately on that thread; otherwise each track buffers
// one to thirty seconds of DTS coverage before synchronized playback begins.
// Once started, temporary starvation preserves that generation's media clock;
// late packets resume immediately without refilling or reanchoring the cache.
class PacketQueue final {
 public:
  // Borrows consumer until Stop completes and never calls consumer.Stop().
  // Notifications are serialized on the queue thread without holding the input
  // queue lock. Consumer callbacks may Abort, but must not Stop or destroy this
  // queue. EOF/interruption is delivered after cached tail packets; source
  // stopped/failed discards the cache before delivering its end notification.
  // Invalid durations throw before the thread starts. Asynchronous scheduling
  // errors are retained in error()/state(); an open generation receives one
  // failed end notification after the error has been published.
  PacketQueue(std::chrono::milliseconds cache_duration, Sink& consumer);
  ~PacketQueue();

  PacketQueue(const PacketQueue&) = delete;
  PacketQueue& operator=(const PacketQueue&) = delete;

  // Copy/reference acquisition can throw synchronously. Calls after Abort are
  // ignored. Streams must precede packets; every replacement generation must
  // have a preceding reset. Stale and duplicate notifications are ignored.
  void OnStreamsReady(const StreamsReady& streams);
  void OnPacket(const PacketReady& packet);
  void OnTimelineReset(const TimelineReset& reset);
  void OnInputEnded(const StreamEnded& end);

  // Discards pending input and wakes the scheduler without waiting for it.
  // Thread-safe, including from callbacks or decoder workers. It does not
  // interrupt an already running consumer callback.
  void Abort() noexcept;
  // Abort and join; idempotent. Call outside the queue thread. No consumer
  // callbacks remain after return. Stop upstream delivery before destruction.
  void Stop() noexcept;

  // Thread-safe queue snapshots. kDraining lasts from processing the source end
  // command until its consumer notification returns. kEnded means cached
  // delivery is complete, not that the downstream consumer has finished.
  // A reset publishes its new generation immediately, even before replacement
  // streams arrive. Abort and Stop preserve a failure and its diagnostic;
  // snapshots remain readable.
  PacketSinkState state() const noexcept;
  std::uint64_t generation() const noexcept;
  std::string error() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_

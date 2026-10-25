#ifndef MW_STREAMER_PLATFORM_WRITE_SIGNAL_H_
#define MW_STREAMER_PLATFORM_WRITE_SIGNAL_H_

#include <atomic>
#include <memory>
#include <utility>

#include "mw/streamer/platform/wait_signal.h"

namespace mw::streamer::internal {

// A producer's capacity revision, optionally waking a shared event loop. Each
// queue/track has its own instance: unrelated read/worker/peer-track events
// must not masquerade as newly available capacity on this track.
class WriteSignal final {
 public:
  explicit WriteSignal(std::weak_ptr<WaitSignal> wake = {})
      : wake_(std::move(wake)) {}

  WaitSignal::Revision GetRevision() const {
    return revision_.load(std::memory_order_acquire);
  }

  void Notify() {
    revision_.fetch_add(1, std::memory_order_release);
    if (const auto wake = wake_.lock()) wake->Notify();
  }

 private:
  const std::weak_ptr<WaitSignal> wake_;
  std::atomic<WaitSignal::Revision> revision_{0};
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PLATFORM_WRITE_SIGNAL_H_

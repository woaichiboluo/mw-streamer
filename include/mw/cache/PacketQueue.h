#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace toolkit {
class EventPoller;
}

namespace mw::cache {

enum class PacketQueueState {
  Filling,
  Playing,
  Paused,
  Starved,
  Stopped,
};

struct PacketStream {
  int stream_index = -1;
  AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
  AVRational time_base{0, 1};
};

class PacketQueue final {
 public:
  using Ptr = std::shared_ptr<PacketQueue>;
  using OnPacket =
      std::function<void(std::uint64_t generation, const AVPacket* packet)>;
  using OnState =
      std::function<void(std::uint64_t generation, PacketQueueState state)>;
  using OnTimelineReset =
      std::function<void(std::uint64_t generation)>;

  explicit PacketQueue(
      std::chrono::milliseconds cache_duration,
      std::shared_ptr<toolkit::EventPoller> poller = nullptr);
  ~PacketQueue();

  PacketQueue(const PacketQueue&) = delete;
  PacketQueue& operator=(const PacketQueue&) = delete;

  // All callbacks are serialized on the owner poller. AVPacket ownership
  // remains with the queue and is valid only for the duration of OnPacket.
  void setOnPacket(OnPacket callback);
  void setOnState(OnState callback);
  void setOnTimelineReset(OnTimelineReset callback);

  // One audio stream and one video stream are required. Calling setStreams
  // with a newer generation atomically clears the previous timeline.
  void setStreams(std::uint64_t generation,
                  std::vector<PacketStream> streams);

  // The packet is cloned before this method returns. Invalid packets, unknown
  // streams, decreasing per-stream DTS, and stale generations are discarded.
  // The return value reports synchronous validation and clone success; Poller
  // side timeline validation may still discard a queued packet.
  bool input(std::uint64_t generation, const AVPacket* packet);

  // Marks the generation as having no more input. Cached packets keep playing,
  // including the unmatched tail of either track.
  void endInput(std::uint64_t generation);

  // Playback controls affect only cached output. Callers controlling a
  // PlayerProxy should apply the same pause and rate to both components.
  void pause(bool paused);
  void setPlaybackRate(double rate);

  // Clears cached packets, cancels the pending output task, and rejects future
  // input until setStreams is called again.
  void stop();

  PacketQueueState state() const noexcept;
  std::uint64_t generation() const noexcept;
  std::size_t packetCount() const noexcept;
  std::shared_ptr<toolkit::EventPoller> getPoller() const;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::cache

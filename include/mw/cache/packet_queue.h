#ifndef MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_
#define MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include "mw/ffmpeg/packet.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::cache {

enum class PacketQueueState {
  kFilling,
  kPlaying,
  kPaused,
  kStarved,
  kStopped,
};

struct PacketStream {
  int stream_index = -1;
  AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
  AVRational time_base{0, 1};
};

class PacketQueue final {
 public:
  using Ptr = std::shared_ptr<PacketQueue>;
  using OnPacket = std::function<void(std::uint64_t generation,
                                      const ffmpeg::Packet& packet)>;
  using OnState =
      std::function<void(std::uint64_t generation, PacketQueueState state)>;
  using OnTimelineReset = std::function<void(std::uint64_t generation)>;
  using OnGenerationEnd = std::function<void(std::uint64_t generation)>;

  // A zero duration enables immediate forwarding. Other supported durations
  // are from one to thirty seconds, inclusive.
  explicit PacketQueue(std::chrono::milliseconds cache_duration,
                       std::shared_ptr<toolkit::EventPoller> poller = nullptr);
  ~PacketQueue();

  PacketQueue(const PacketQueue&) = delete;
  PacketQueue& operator=(const PacketQueue&) = delete;

  // All callbacks are serialized on the owner poller. Packet ownership remains
  // with the queue and is valid only for OnPacket. Copy or call Ref to retain
  // it.
  void SetOnPacket(OnPacket callback);
  void SetOnState(OnState callback);
  // Called when a newer generation atomically discards the previous timeline.
  // Downstream timeline-local state should be flushed here.
  void SetOnTimelineReset(OnTimelineReset callback);
  // Called exactly once after EndInput when every cached packet in that
  // generation has been delivered. Replacing a generation does not emit this
  // callback for the discarded generation. Downstream decoders may drain here.
  void SetOnGenerationEnd(OnGenerationEnd callback);

  // One audio stream, one video stream, or one of each is required. Calling
  // SetStreams with a newer generation atomically clears the previous
  // timeline.
  void SetStreams(std::uint64_t generation, std::vector<PacketStream> streams);

  // The packet is referenced before this method returns. Invalid packets,
  // unknown streams, decreasing per-stream DTS, and stale generations are
  // discarded. The return value reports synchronous validation and reference
  // success; Poller side timeline validation may still discard a queued packet.
  bool Input(std::uint64_t generation, const ffmpeg::Packet& packet);

  // Marks the generation as having no more input. Cached packets keep playing,
  // including the unmatched tail of any configured track.
  void EndInput(std::uint64_t generation);

  // Callers controlling a PlayerProxy should apply the same pause and rate to
  // both components. Immediate-forwarding mode drops packets received while
  // paused instead of buffering them, and playback rate does not pace packets.
  void Pause(bool paused);
  void SetPlaybackRate(double rate);

  // Clears cached packets, cancels the pending output task, and rejects future
  // input until SetStreams is called again.
  void Stop();

  PacketQueueState state() const noexcept;
  std::uint64_t generation() const noexcept;
  std::size_t packet_count() const noexcept;
  std::shared_ptr<toolkit::EventPoller> poller() const;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace mw::streamer::cache

#endif  // MW_STREAMER_INCLUDE_MW_CACHE_PACKET_QUEUE_H_

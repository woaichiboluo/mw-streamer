#ifndef MW_STREAMER_INPUT_FFMPEG_PACKET_READER_H_
#define MW_STREAMER_INPUT_FFMPEG_PACKET_READER_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

#include "mw/streamer/ffmpeg/packet.h"

namespace mw::streamer::internal {

// One demux worker and one consumer. Pop retains the packet's byte reservation
// until Release, including time spent in the consumer's per-track queues.
// Read precedes the bounded queue push. The worker may hold one additional
// packet while waiting for queue or byte capacity.
// EAGAIN yields; ENOMEM retries every 5 ms, ending after 20 consecutive
// failures. EOF and AVERROR_EXIT drain accepted packets before ending.
// Stop interrupts Read and wakes queue/retry waits. Serialize Stop/destruction.
class FfmpegPacketReader final {
 public:
  using Read = std::function<int(ffmpeg::Packet&)>;
  // Must cancel an in-flight Read without waiting for the consumer or throwing.
  using CancelRead = std::function<void()>;
  static constexpr std::size_t kPacketCapacity = 256;

  FfmpegPacketReader(Read read, CancelRead cancel_read, std::size_t byte_limit);
  ~FfmpegPacketReader();
  FfmpegPacketReader(const FfmpegPacketReader&) = delete;
  FfmpegPacketReader& operator=(const FfmpegPacketReader&) = delete;

  // EOF and Stop return false. An I/O failure is propagated after queued data.
  // An empty queue blocked by retained bytes fails instead of deadlocking.
  bool Pop(ffmpeg::Packet& packet);
  // Non-blocking consumption for an asynchronous decode/delivery pump. A
  // pending reservation is normal backpressure while decoders can release it.
  bool TryPop(ffmpeg::Packet& packet);
  bool finished() const noexcept;
  bool budget_blocked() const noexcept;
  void Release(std::size_t bytes) noexcept;
  void RequestStop() noexcept;
  void Stop() noexcept;
  std::size_t outstanding_bytes() const noexcept;
  std::size_t high_water_bytes() const noexcept;

 private:
  void Run() noexcept;

  Read read_;
  CancelRead cancel_read_;
  const std::size_t byte_limit_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<ffmpeg::Packet> packets_;
  std::size_t outstanding_bytes_ = 0;
  std::size_t high_water_bytes_ = 0;
  bool stopping_ = false;
  bool finished_ = false;
  std::size_t pending_bytes_ = 0;
  std::exception_ptr error_;
  std::thread worker_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INPUT_FFMPEG_PACKET_READER_H_

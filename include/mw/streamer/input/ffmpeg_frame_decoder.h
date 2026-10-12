#ifndef MW_STREAMER_INPUT_FFMPEG_FRAME_DECODER_H_
#define MW_STREAMER_INPUT_FFMPEG_FRAME_DECODER_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "mw/streamer/ffmpeg/decoder.h"

namespace mw::streamer::internal {

// One decoder worker per track. Receive precedes the bounded output push, so
// the worker can hold one decoded frame beyond the queue. The input watermark
// guides dispatch without blocking it behind a full track. The reader owns the
// shared packet budget and must outlive this object and its Release callback.
// Release and Notify can run on the caller or decoder thread and must be thread
// safe.
class FfmpegFrameDecoder final {
 public:
  using Release = std::function<void(std::size_t)>;
  using Notify = std::function<void()>;
  using ParserRepeatPict = std::function<int()>;
  static constexpr std::size_t kPacketWatermark = 2;
  static constexpr std::size_t kFrameCapacity = 8;

  FfmpegFrameDecoder(std::unique_ptr<ffmpeg::Decoder> decoder, Release release,
                     Notify notify, ParserRepeatPict parser_repeat_pict = {});
  ~FfmpegFrameDecoder();
  FfmpegFrameDecoder(const FfmpegFrameDecoder&) = delete;
  FfmpegFrameDecoder& operator=(const FfmpegFrameDecoder&) = delete;

  // Always consumes packet ownership and its reader reservation. false means
  // EOF or Stop has already rejected input; that reservation is released here.
  bool Push(ffmpeg::Packet packet);
  // Submit EOF once, after all selected packets have been dispatched.
  void Finish() noexcept;
  // Nonblocking. A worker error is rethrown only after queued frames are read.
  // Frames preserve decoder metadata; no clock or timestamp mapping is applied.
  bool TryReceive(ffmpeg::Frame& frame);
  void ThrowIfFailed() const;
  // Stop wakes both queue waits and discards queued packets and frames. Calls
  // to Stop/destruction must be serialized; RequestStop is thread safe.
  void RequestStop() noexcept;
  void Stop() noexcept;
  // Call Stop before replacing the reader callback's target. Restart flushes
  // the same decoder/device, resets terminal state, and starts a new worker.
  void Restart();
  bool finished() const noexcept;
  // End of the drained track in its source time base. Unknown before normal
  // EOF, after Stop/restart, or if the last frame has no usable media
  // timestamp.
  std::int64_t end_pts() const noexcept;
  bool wants_packet() const noexcept;
  bool blocked() const noexcept;
  bool waiting_for_packet() const noexcept;
  std::size_t queued_bytes() const noexcept;

 private:
  void Run() noexcept;
  void ReleasePacket(std::size_t bytes, bool queued = true) noexcept;
  void NotifyChanged() noexcept;

  std::unique_ptr<ffmpeg::Decoder> decoder_;
  Release release_;
  Notify notify_;
  ParserRepeatPict parser_repeat_pict_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<ffmpeg::Packet> packets_;
  std::deque<ffmpeg::Frame> frames_;
  std::size_t queued_bytes_ = 0;
  bool input_finished_ = false;
  bool worker_finished_ = false;
  bool stopping_ = false;
  bool waiting_for_input_ = false;
  std::int64_t end_pts_ = AV_NOPTS_VALUE;
  std::exception_ptr error_;
  std::thread worker_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INPUT_FFMPEG_FRAME_DECODER_H_

#ifndef MW_STREAMER_DECODER_AV_DECODER_H_
#define MW_STREAMER_DECODER_AV_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

struct AvDecoderConfig {
  // Empty selects by codec id. A specified decoder must exist and match the
  // track/device; no other decoder is tried on failure.
  std::string decoder_name;
  // Maximum queued input items, including Packet, EOF and Reset. The worker
  // can additionally hold one input item. Must be positive.
  std::size_t packet_capacity = 12;
  // Maximum queued output items, including EOF. The worker can additionally
  // hold one decoded frame while waiting for room. Must be positive.
  std::size_t frame_capacity = 8;
  // Software: 0 selects automatic threading. CUDA maps 0 to 2; explicit
  // counts and FFmpeg's default threading mode are preserved.
  int thread_count = 0;
};

enum class AvDecoderReadStatus { kFrame, kEof, kNull };

struct AvDecoderReadResult {
  AvDecoderReadStatus status = AvDecoderReadStatus::kNull;
  std::optional<ffmpeg::Frame> frame;
  int stream_index = -1;
  AVRational time_base{0, 1};
  // Source-time endpoint of the final frame, only for EOF; unknown when timing
  // cannot be determined. EOF is a separate item, never an empty AVFrame.
  std::int64_t end_pts = AV_NOPTS_VALUE;
};

// One decoder/worker per track, with Packet input and Frame output
// queues. Does not read a Proxy, select tracks, reconnect, filter or map
// clocks. One packet producer and one nonblocking frame consumer may run
// concurrently. Stop/destruction must be serialized; finish callers before
// destruction.
class MW_STREAMER_API AvDecoder final {
 public:
  // A terminal worker failure is retained in error() and notified once outside
  // locks, on the decoder worker. Observers may query, read or RequestStop, but
  // must not call Stop or destroy this object. Callback exceptions are logged.
  using OnError = std::function<void(std::exception_ptr)>;

  // Copies stream parameters, opens the codec and starts its worker. Invalid
  // arguments/opening/thread creation failures throw synchronously.
  explicit AvDecoder(const ffmpeg::StreamInfo& stream,
                     AvDecoderConfig config = {});
  // Shares the caller's device reference; never creates a hardware device.
  // A CPU device uses software decoding. CUDA video stays on the GPU and
  // cannot fall back to software; audio uses CPU decoding. Use the same device
  // instance for all hardware decoders/encoders. Returned frames retain it.
  AvDecoder(const ffmpeg::StreamInfo& stream,
            const ffmpeg::HwDeviceContext& device, AvDecoderConfig config = {});
  ~AvDecoder();
  AvDecoder(const AvDecoder&) = delete;
  AvDecoder& operator=(const AvDecoder&) = delete;
  AvDecoder(AvDecoder&&) = delete;
  AvDecoder& operator=(AvDecoder&&) = delete;

  // Thread-safe registration/replacement. Register before submitting packets;
  // failures that have already occurred are not replayed.
  void SetOnError(OnError callback);

  // Never waits for queue space. Copies a packet reference only on acceptance;
  // false preserves the caller's packet and means full, stopped or failed.
  // An empty/wrong-track packet throws. After accepted EOF, packets and
  // repeated EOF throw until TryReset succeeds. All input items share
  // packet_capacity.
  bool TryPushPacket(const ffmpeg::Packet& packet);
  // Queues a drain behind accepted packets, publishing delayed frames then EOF.
  // A rejected EOF changes no state and can be retried. The worker remains
  // available; queue Reset before submitting more media.
  bool TryPushEof();
  // Queues an ordered codec flush for an input reopen. Discards codec delay
  // and clears drain/timing state, while already decoded output remains FIFO.
  // Does not publish an output item. False means full, stopped or failed and
  // leaves the accepted EOF state unchanged; retry before submitting more
  // media.
  bool TryReset();
  // Never waits for media. Returns queued frames/EOF in order, then kNull.
  // Frames own their storage independently of codec lifetime. PTS uses
  // best_effort_timestamp in the original stream time base.
  // Failure is observed through error()/OnError, not disguised as normal EOF.
  AvDecoderReadResult TryReadFrame();
  std::size_t buffered_packets() const;
  std::size_t buffered_frames() const;
  std::exception_ptr error() const;
  // Immutable track description, borrowed for this decoder's lifetime.
  const ffmpeg::StreamInfo& stream_info() const;

  // Wakes input/output waits. Stop joins and discards queued data; returned
  // frames remain valid. No callbacks after Stop returns. Cannot restart.
  void RequestStop() noexcept;
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_DECODER_AV_DECODER_H_

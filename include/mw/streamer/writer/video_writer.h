#ifndef MW_STREAMER_WRITER_VIDEO_WRITER_H_
#define MW_STREAMER_WRITER_VIDEO_WRITER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace mw::streamer {

struct VideoWriterSourceTime {
  std::int64_t pts;
  AVRational time_base;
};

// Standalone local MP4 writer. Requires Init and an explicitly shared device.
// Serialize Open/Write/Finish/destruction; RequestStop and source_start_time
// may run concurrently. The writer adds no worker: it reuses AvEncoder's
// workers and synchronously muxes their packets under one lock. Writes wait
// for encoder capacity; they never discard a frame on backpressure.
class MW_STREAMER_API VideoWriter final {
 public:
  explicit VideoWriter(const ffmpeg::HwDeviceContext& device);
  // Cancels and joins encoding, then closes the file best-effort. Call Finish
  // explicitly to drain all media and observe file-finalization failures.
  ~VideoWriter();
  VideoWriter(const VideoWriter&) = delete;
  VideoWriter& operator=(const VideoWriter&) = delete;

  // Single-use session. Local .mp4 path, overwritten; parent must exist.
  // NONE disables an input track, as in AvEncoderConfig. Video dimensions and
  // FPS must be explicit. No resize/FPS conversion, realtime pacing, timeline
  // repair or silence generation. Audio is converted to the configured target.
  // Parameters/layout are copied during this call. Failures throw.
  void Open(std::string_view path, const AvEncoderConfig& config);
  // Accept immutable decoded/processed frames with valid source PTS/time_base.
  // Each enabled track's PTS must strictly increase. CPU and CUDA video use
  // AvEncoder's device/layout rules; CUDA callers need not bind a context.
  // Success means accepted, not encoded or durable on disk. The earliest first
  // source presentation time of the enabled tracks becomes the common origin.
  // Until all first frames arrive, compressed packets are buffered (not raw
  // images); this startup storage grows with the initial track imbalance.
  // Never waits for a future call on the other track. Source headers/buffers,
  // gaps, relative offsets, codec reorder delay and padding are preserved.
  // A processing/IO failure is terminal and rethrown by later writes/Finish.
  void WriteVideo(const ffmpeg::Frame& frame);
  void WriteAudio(const ffmpeg::Frame& frame);
  // Caller must submit business-buffered frames first. Requires a frame from
  // every enabled track, then drains resampling, codecs and MP4 in that order.
  // Successful repeated Finish is a no-op. Cancellation/missing tracks and
  // encoding/IO failures throw; partial files are retained for diagnosis.
  void Finish();
  // Cooperative: wakes capacity waits and rejects further writes/packets.
  // Does not join or forcibly interrupt an in-progress local filesystem write.
  void RequestStop() noexcept;
  // Empty until all enabled tracks have submitted a first frame. Original
  // source time corresponding to output presentation zero; use this same
  // mapping for offline algorithm metadata. Encoder time-base rounding applies.
  std::optional<VideoWriterSourceTime> source_start_time() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_WRITER_VIDEO_WRITER_H_

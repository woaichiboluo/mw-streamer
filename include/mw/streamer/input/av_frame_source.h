#ifndef MW_STREAMER_INPUT_AV_FRAME_SOURCE_H_
#define MW_STREAMER_INPUT_AV_FRAME_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer {

enum class AvFrameSourceReadStatus { kFrame, kEof, kNull };

struct AvFrameSourceReadResult {
  AvFrameSourceReadStatus status = AvFrameSourceReadStatus::kNull;
  // One output period's picture and audio samples, ready for Scheduler. Missing
  // input tracks stay empty. Frame buffers retain their CPU/CUDA storage.
  std::optional<ffmpeg::Frame> video;
  std::optional<ffmpeg::Frame> audio;
  std::uint64_t generation = 0;
  std::uint64_t frame_index = 0;
  // Source presentation time in microseconds, not the Scheduler's output clock.
  // Audio determines timing when a new audio block is present.
  // Each native AVFrame also retains its filter output time_base/PTS.
  std::int64_t pts_us = AV_NOPTS_VALUE;
  std::int64_t duration_us = 0;
};

// Pulls decoded frames from Decoder caches, filters and
// bundles audio/video on one worker, then buffers complete output periods.
// Video only changes frame rate; audio uses async resampling, configured sample
// format/layout and output-period sample cadence. No deinterlacing or resizing.
// After a track ends, its last frame/block is repeated while the other
// continues. No packet demuxing/reconnection, encoding or Scheduler clock
// ownership.
class MW_STREAMER_API AvFrameSource final {
 public:
  // Once on terminal worker failure, outside locks. Query/read/RequestStop are
  // allowed; Stop/destruction on this callback's worker are forbidden.
  // Failure retains the original exception; reads never turn it into EOF.
  using OnFailed = std::function<void(std::exception_ptr)>;

  // Borrow optional Decoder tracks (at least one). Copies their descriptions
  // and the output config, including channel layout. Decoders must outlive
  // this source; stop the source before destroying them. One frame consumer
  // may run concurrently; lifecycle calls must be serialized.
  // The initial container start time, if known, aligns both filter graphs.
  // Later generations derive a shared origin from their first decoded frames.
  AvFrameSource(AvDecoder* video, AvDecoder* audio, AvEncoderConfig config = {},
                std::int64_t start_time_us = AV_NOPTS_VALUE,
                OnFailed on_failed = {});
  ~AvFrameSource();
  AvFrameSource(const AvFrameSource&) = delete;
  AvFrameSource& operator=(const AvFrameSource&) = delete;
  AvFrameSource(AvFrameSource&&) = delete;
  AvFrameSource& operator=(AvFrameSource&&) = delete;

  // Nonblocking cache-only read; never decodes or runs filters on the caller.
  // Initially/new generation prebuffers min(4, capacity) periods. EOF or a
  // discontinuity releases a short segment too; running underflow is kNull.
  // Each normally drained generation reports EOF after its final bundle.
  // New generations never mix tracks. Returned frames survive Stop/reconnect.
  AvFrameSourceReadResult TryReadFrame();
  // Default final queue capacity is max(1, floor(fps) / 4).
  std::size_t frame_capacity() const;
  std::size_t buffered_frames() const;
  std::exception_ptr error() const;
  void RequestStop() noexcept;
  // Wakes full-cache/input polling waits, joins, discards queued bundles. Does
  // not stop borrowed Decoders; no source callbacks after returning.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_FRAME_SOURCE_H_

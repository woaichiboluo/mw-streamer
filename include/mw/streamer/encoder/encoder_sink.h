#ifndef MW_STREAMER_ENCODER_ENCODER_SINK_H_
#define MW_STREAMER_ENCODER_ENCODER_SINK_H_

#include <cstddef>
#include <memory>
#include <string>

#include "mw/streamer/encoder/config.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

enum class EncoderSinkState {
  kIdle,
  kRunning,
  kDraining,
  kEnded,
  kFailed,
  kStopped,
};

// Owns a bounded frame queue and one worker for audio/video encoding. Frame
// submission retains read-only buffer references; encoding never runs on the
// upstream thread. All downstream Sink calls are serialized in worker
// order and fan out shared packet buffers to exclusively owned consumers.
// Source metadata declares at most one audio and one video track; actual
// encoding parameters come from the first frame for each track. Audio uses
// the existing 48 kHz interleaved float32 contract. Video supports CPU/CUDA
// frames through the existing encoder, without conversion or fallback.
// This sink does not synchronize tracks, pace frames, or synthesize standby.
// Submission/encoding errors fail this sink and are available through error();
// explicit FatalError and downstream fatal reports also request Pipeline stop.
// Ordinary downstream failures remain owned by their respective sinks.
class EncoderSink final : public Sink {
 public:
  explicit EncoderSink(std::string id, EncoderSinkConfig config = {});
  ~EncoderSink() override;

  EncoderSink(const EncoderSink&) = delete;
  EncoderSink& operator=(const EncoderSink&) = delete;

  void OnStreamsReady(const FrameStreamsReady& streams) override;
  void OnAudioFrame(const FrameReady& frame) override;
  void OnVideoFrame(const FrameReady& frame) override;
  // Discards queued frames from older generations and recreates encoders.
  // Downstream reset precedes the next encoded StreamsReady; no old packet
  // is emitted after that reset. Discarded codec delay is not drained.
  void OnTimelineReset(const TimelineReset& reset) override;
  // EOF drains codecs before downstream EOF. A declared track with no frames
  // is an error. Interruption discards codec delay and permits a new
  // generation.
  void OnInputEnded(const StreamEnded& end) override;

  // Stops admission, discards pending frames, joins the worker, then stops all
  // consumers. Does not manufacture EOF. Upstream delivery must have stopped;
  // never invoke from callbacks or owned worker. Idempotent; preserves failure.
  void Stop() noexcept override;
  EncoderSinkState state() const noexcept;
  std::string error() const;
  std::size_t queue_depth() const;

 private:
  NodeSnapshot GetOwnPerformance() const override;
  void HandleFatalError(const std::string& error) noexcept override;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_ENCODER_ENCODER_SINK_H_

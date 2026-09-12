#ifndef MW_STREAMER_INCLUDE_MW_DECODER_DECODER_SINK_H_
#define MW_STREAMER_INCLUDE_MW_DECODER_DECODER_SINK_H_

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "mw/decoder/config.h"
#include "mw/sink/sink.h"

namespace mw::streamer {

// Owns a PacketQueue with its own scheduling thread, separate audio/video
// storage and one shared clock. No caller-owned executor is required.
// Each present track has its own decode queue and worker; packet submission
// never performs decoding on the caller's thread. At most one audio and one
// video track are supported, with stable source parameters across generations.
// Audio is resampled here to 48 kHz interleaved float32, preserving channel
// layout. Video retains its decoder format and hardware context.
// Offline sources bypass the timed PacketQueue, require zero cache duration,
// and block at each bounded decode queue instead of dropping packets. Audio
// and video callbacks still run on their respective workers.
// Downstream FatalError fails this sink and reports through SetOnFatalError;
// ordinary exceptions only fail this sink. Shutdown never joins a worker from
// its own callback: the fatal callback only requests external control work.
class DecoderSink final : public Sink {
 public:
  explicit DecoderSink(std::string id, DecoderSinkConfig config);
  ~DecoderSink() override;

  DecoderSink(const DecoderSink&) = delete;
  DecoderSink& operator=(const DecoderSink&) = delete;

  // Missing consumers fail the sink when the first stream is configured.
  void OnStreamsReady(const StreamsReady& streams) noexcept override;
  void OnPacket(const PacketReady& packet) noexcept override;
  void OnTimelineReset(const TimelineReset& reset) noexcept override;
  void OnInputEnded(const StreamEnded& end) noexcept override;

  void RequestStop() noexcept override;

  // Abort pending work, wait for both workers, then stop all downstream sinks.
  // Upstream delivery must be stopped first. Must run outside PacketQueue,
  // decode and downstream callback threads. Destruction follows the same
  // contract.
  void Stop() noexcept override;
  PacketSinkState state() const noexcept;
  std::string error() const;

 private:
  using Sink::downstream;
  using Sink::ReportFatalError;
  using Sink::StartMessages;
  using Sink::StopDownstream;

  NodeSnapshot GetOwnPerformance() const override;
  void HandleFatalError(const std::string& error) noexcept override;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INCLUDE_MW_DECODER_DECODER_SINK_H_

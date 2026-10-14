#ifndef MW_STREAMER_OUTPUT_FRAME_SINK_H_
#define MW_STREAMER_OUTPUT_FRAME_SINK_H_

#include <cstdint>
#include <future>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/media_frame.h"

namespace mw::streamer {

struct OutputFrame {
  // Source PTS remains unchanged. Counters identify channel ticks for
  // monitoring; each frame sink owns its continuous playback/encoding timestamp
  // policy.
  MediaFrame frame;
  std::uint64_t frame_number = 0;
  std::uint64_t audio_sample_index = 0;
  int audio_samples = 0;  // Per channel for this tick.
};

// Initialize and Send are externally serialized. Frame sinks own their bounded
// queues/workers and finish their resources in their destructor. Output holds
// shared ownership; releasing its reference need not destroy the frame sink.
class MW_STREAMER_API FrameSink {
 public:
  virtual ~FrameSink() = default;

  // Copy any retained configuration, including its borrowed channel layout.
  // Configuration defines the channel output dimensions and media formats.
  // Hardware contexts, when needed, belong to the concrete frame sink.
  // Do not wait for channel frames here. Return false after logging and
  // cleaning up a failed attempt; a later call may retry. Successful
  // initialization is called only once, and its resources live until
  // destruction.
  [[nodiscard]] virtual bool Initialize(
      const AvEncoderConfig& format) noexcept = 0;
  // Return promptly with a valid future. All frame sinks receive a frame before
  // Output waits for results. Media buffers are shared and read-only; mutation
  // requires private writable buffers. Each value owns independent AVFrames.
  // true means accepted or intentionally dropped by this frame sink's policy;
  // false/exception removes this frame sink from subsequent fan-out.
  // Completion is an admission/backpressure barrier, not physical completion.
  virtual std::future<bool> Send(OutputFrame frame) = 0;
  // Futures must come from promises/packaged tasks: destroying them must not
  // wait for worker completion. Deferred futures are unsupported. Outstanding
  // work may remain after the channel stops sending; finish it on destruction.
  // Fixed after Initialize. true requires bounded backpressure driven by
  // device playback progress, including bounded startup prefill.
  virtual bool has_synchronization_clock() const noexcept { return false; }
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_OUTPUT_FRAME_SINK_H_

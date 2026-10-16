#ifndef MW_STREAMER_PIPELINE_OFFLINE_PIPELINE_H_
#define MW_STREAMER_PIPELINE_OFFLINE_PIPELINE_H_

#include <exception>
#include <functional>
#include <memory>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/filter/av_media_filter.h"
#include "mw/streamer/input/av_media_source.h"

namespace mw::streamer {

struct OfflinePipelineConfig {
  AvInputConfig input;
  AvDecoderConfig video_decoder;
  AvDecoderConfig audio_decoder;
};

enum class OfflinePipelineState {
  kStopped,
  kStarting,
  kRunning,
  kDraining,
  kEnded,
  kStopping,
  kFailed,
};

struct OfflinePipelineStatus {
  OfflinePipelineState state = OfflinePipelineState::kStopped;
  AvMediaSourceStatus input;
  std::exception_ptr error;
};

// Local-file processing at the speed allowed by decoding/business callbacks.
// Owns MediaSource and a filter; there is no loop, retry, media clock or
// output. One consumer serializes audio/video callbacks, preserving each
// track's FIFO order. It reads the next frame only after the preceding callback
// returns. Source/decoder queues bound prefetch and propagate backpressure
// upstream. Requires Init; Stop/destroy before Shutdown. Serialize
// lifecycle/configuration externally. Snapshots and RequestStop may run
// concurrently with all workers.
class MW_STREAMER_API OfflinePipeline final {
 public:
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  using OnError = std::function<void(std::exception_ptr)>;

  explicit OfflinePipeline(OfflinePipelineConfig config = {});
  OfflinePipeline(const ffmpeg::HwDeviceContext& device,
                  OfflinePipelineConfig config = {});
  ~OfflinePipeline();
  OfflinePipeline(const OfflinePipeline&) = delete;
  OfflinePipeline& operator=(const OfflinePipeline&) = delete;
  OfflinePipeline(OfflinePipeline&&) = delete;
  OfflinePipeline& operator=(OfflinePipeline&&) = delete;

  // Configure only before Start/after Stop; lifecycle is owned by this
  // pipeline. Return values of OnVideo/OnAudio are discarded, including empty
  // frames. OnStart/OnVideo/OnAudio/OnReset/OnStop run serially on the consumer
  // worker. After EOF from every present track, calls OnReset once, then
  // OnStop. Input/processing failures or cancellation before EOF skip OnReset.
  // OnStop follows a successful OnStart, even after processing/reset failures.
  AvMediaFilter& media_filter();
  // Observes input packets on the source controller, concurrently with filters.
  // Input storage is read-only; Ref() retains it. Exceptions fail the pipeline.
  void SetOnInputPacket(OnPacket callback);
  // First unrecoverable error, including filter/reset/stop/startup failures.
  // Observer exceptions are contained. All callbacks may query/RequestStop,
  // but must not configure, Start/Stop or destroy the pipeline. Filter
  // callbacks additionally must not reenter the filter. Stop waits for every
  // callback.
  void SetOnError(OnError callback);

  // Only regular local-file paths, including relative paths; URLs are rejected.
  // Asynchronous: true confirms worker startup, not successful input opening.
  // Invalid paths/startup failures return false and report OnError. Call Stop
  // before reuse, also after Ended/Failed. No implicit retry or looping.
  [[nodiscard]] bool Start(std::string_view path);
  OfflinePipelineState state() const;
  AvMediaSourceStatus input_status() const;
  OfflinePipelineStatus status() const;
  std::exception_ptr error() const;
  void RequestStop() noexcept;
  // Cancels I/O/queue waits, then joins the consumer and source. A synchronous
  // business callback must return before Stop can finish. Normal Ended is only
  // published after Reset/Stop and source cleanup; final states persist until
  // the next Start. No callbacks after this returns.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_OFFLINE_PIPELINE_H_

#ifndef MW_STREAMER_INPUT_AV_MEDIA_INPUT_H_
#define MW_STREAMER_INPUT_AV_MEDIA_INPUT_H_

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/input/av_frame_source.h"
#include "mw/streamer/input/av_input_proxy.h"

namespace mw::streamer {

enum class AvMediaInputMode { kBatch, kLive };

struct AvMediaInputConfig {
  AvMediaInputMode mode = AvMediaInputMode::kLive;
  AvInputProxyConfig proxy;
  AvDecoderConfig video_decoder;
  AvDecoderConfig audio_decoder;
  // Used only in Live mode. Batch exposes unfiltered source frames.
  // This is input conversion configuration, not channel track selection.
  // NONE audio is normalized internally to FLTP; source audio is still
  // consumed, while Scheduler may disable its output with the original config.
  AvEncoderConfig output;
};

enum class AvMediaInputState {
  kStopped,
  kConnecting,
  // Selected decoders/source are initialized; data may still be prefilling.
  kRunning,
  kWaitingRetry,
  // The final packet EOF has been dispatched; consumers must drain output.
  kDraining,
  // Consumers received EOF from every selected batch track/the live source.
  kEnded,
  kFailed,
};

struct AvMediaInputStatus {
  AvMediaInputState state = AvMediaInputState::kStopped;
  std::uint64_t generation = 0;
  std::uint64_t retry_count = 0;
  std::chrono::seconds retry_delay{0};
  // Recoverable connection error in WaitingRetry; first terminal exception in
  // Failed. Cleared after successful initialization/reconnection.
  std::exception_ptr error;
  std::shared_ptr<const AvInputInfo> info;
};

// Owns Proxy, selected single-track Decoders and optional live FrameSource.
// One controller/packet-dispatch worker serializes external state callbacks.
// Existing child components own their caches; this class adds no media queue.
// Mode is fixed at construction. Lifecycle/configuration calls are serialized
// externally. One consumer per Batch track or one Live consumer may read while
// stopping; finish old calls before Start/destruction.
class MW_STREAMER_API AvMediaInput final {
 public:
  // On the controller worker, outside locks. Query/read/RequestStop are
  // allowed. Stop/Start/configuration/destruction from this callback are
  // forbidden. Child callbacks only post events; observer exceptions are
  // logged.
  using OnStateChanged = std::function<void(const AvMediaInputStatus&)>;

  explicit AvMediaInput(AvMediaInputConfig config = {});
  // Retains the caller's shared CPU/CUDA device; never creates another device.
  // Video decoders and downstream encoders can use the same native context.
  AvMediaInput(const ffmpeg::HwDeviceContext& device,
               AvMediaInputConfig config = {});
  ~AvMediaInput();
  AvMediaInput(const AvMediaInput&) = delete;
  AvMediaInput& operator=(const AvMediaInput&) = delete;
  AvMediaInput(AvMediaInput&&) = delete;
  AvMediaInput& operator=(AvMediaInput&&) = delete;

  void SetOnStateChanged(OnStateChanged callback);
  // Returns true after the controller enters its thread; connection and media
  // initialization remain asynchronous, with errors reported through status.
  // Invalid URL/restart or thread creation failure logs and returns false.
  // Stop and finish old reads before restarting; generations restart at zero.
  [[nodiscard]] bool Start(std::string_view url) noexcept;
  // Batch only, nonblocking. Consume every present track to relieve
  // backpressure and complete draining. Absent tracks/opening/failure/Stop
  // return kNull. Source format/time bases/PTS are unchanged. Each generation's
  // EOF is visible. Calling these in Live mode throws instead of stealing
  // FrameSource input.
  AvDecoderReadResult TryReadVideo();
  AvDecoderReadResult TryReadAudio();
  // Live only, nonblocking read of cached converted/bundled audio/video.
  // Calling this in Batch mode throws. EOF is visible for each normally drained
  // generation.
  AvFrameSourceReadResult TryReadFrame();
  AvMediaInputMode mode() const;
  AvMediaInputStatus status() const;
  std::shared_ptr<const AvInputInfo> info() const;

  // Cooperative cancellation of dispatch and all child waits; no joins here.
  void RequestStop() noexcept;
  // Stops dispatch first, then joins/releases Source before Decoders and Proxy.
  // No callbacks after returning; final Failed/Ended states are retained.
  // Already returned CPU/CUDA frames/info remain valid.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_AV_MEDIA_INPUT_H_

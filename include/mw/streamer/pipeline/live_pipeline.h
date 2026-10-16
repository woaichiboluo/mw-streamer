#ifndef MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_
#define MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/filter/av_media_filter.h"
#include "mw/streamer/input/av_media_source.h"
#include "mw/streamer/remuxer/remuxer.h"
#include "mw/streamer/scheduler/realtime_timing.h"
#include "mw/streamer/syncer/syncer.h"

namespace mw::streamer {

struct LivePipelineConfig {
  AvMediaSourceConfig input;
  // Shared target for scheduling and encoding. Video dimensions must match
  // business-filter output; the pipeline does not resize it implicitly.
  AvEncoderConfig encoder;
  SyncerOptions syncer;
  RealtimeConfig realtime;
  RemuxerConfig remuxer;
  // Retains a frame reference during construction. Ignored for audio-only
  // output; empty uses CPU black frames. Must match encoder video dimensions.
  // Pixels remain shared read-only; no implicit resizing is performed.
  std::optional<ffmpeg::Frame> backup_video;
};

enum class LivePipelineState {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
  kFailed,
};

struct LivePipelineStatus {
  LivePipelineState state = LivePipelineState::kStopped;
  AvMediaSourceStatus input;
  // Native Remuxer push statuses; local recordings/publications are excluded.
  std::vector<Remuxer::PushStatus> remuxer;
  std::exception_ptr error;
};

// Owns MediaSource, Scheduler, encoder and Remuxer for one live session.
// Normal input EOF leaves scheduling/encoding/output running. Requires Init
// before Start; destroy the pipeline before Shutdown. Configuration and
// Start/Stop/destruction must be externally serialized. Snapshots and
// RequestStop are safe concurrently with workers and lifecycle calls.
class MW_STREAMER_API LivePipeline final {
 public:
  using OnPacket = std::function<void(const ffmpeg::Packet&)>;
  using OnError = std::function<void(std::exception_ptr)>;

  // CPU by default. The device overload retains the supplied device for both
  // decoding and encoding. Config layouts/standby frames are retained.
  explicit LivePipeline(LivePipelineConfig config);
  LivePipeline(const ffmpeg::HwDeviceContext& device,
               LivePipelineConfig config);
  ~LivePipeline();
  LivePipeline(const LivePipeline&) = delete;
  LivePipeline& operator=(const LivePipeline&) = delete;
  LivePipeline(LivePipeline&&) = delete;
  LivePipeline& operator=(LivePipeline&&) = delete;

  // Configure only before Start or after Stop. Input packets are observed once
  // on the source controller before decoding, with native timestamps. Encoded
  // packets are observed before Remuxer submission, concurrently across the
  // encoding workers. Callbacks must return promptly. Headers and shared
  // payloads are read-only; Ref() retains a packet beyond the callback. Packet
  // callback exceptions fail the session.
  void SetOnInputPacket(OnPacket callback);
  void SetOnOutputPacket(OnPacket callback);
  // First unrecoverable error only, including startup failures. May run on any
  // component worker or lifecycle caller, outside pipeline locks. The original
  // exception is retained; observer exceptions are logged. All callbacks may
  // query snapshots or RequestStop, but must not configure, Start/Stop or
  // destroy the pipeline. Stop externally to finish cleanup before restarting.
  void SetOnError(OnError callback);
  // Business filter configuration only while stopped; lifecycle is owned here.
  AvMediaFilter& media_filter();

  // Forward output registration to Remuxer. Control calls must be serialized
  // with lifecycle calls; dynamic additions during a running session work as
  // on Remuxer. Stop clears registrations; add them again before restarting.
  std::string AddPushUrl(const std::string& url);
  void AddRtspPublish(const std::string& ip, std::uint16_t port,
                      bool tls = false);
  std::string AddHlsPublish(const std::string& ip, std::uint16_t port,
                            bool tls = false);
  Remuxer::OutputInfo output_info() const;

  // Initializes encoder/output synchronously, then starts input/scheduling.
  // Running means local initialization succeeded, not that a remote input or
  // output has connected. False on startup failure or cancellation after
  // rollback. Repeated Start before Stop returns false without changing the
  // session.
  [[nodiscard]] bool Start(std::string_view url);
  LivePipelineState state() const;
  AvMediaSourceStatus input_status() const;
  std::vector<Remuxer::PushStatus> remuxer_status() const;
  // Component snapshots are collected separately, not as one atomic instant.
  LivePipelineStatus status() const;
  std::exception_ptr error() const;
  // Cancels source/scheduling without joining; Stop completes output draining.
  void RequestStop() noexcept;
  // Stops producers, drains encoder, then drains/finalizes Remuxer. Idempotent;
  // no callbacks after return. Failed/error are retained until the next Start.
  void Stop() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_

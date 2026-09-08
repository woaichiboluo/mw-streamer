#ifndef MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_SYNCHRONIZER_SINK_H_
#define MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_SYNCHRONIZER_SINK_H_

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "mw/sink/sink.h"
#include "mw/synchronizer/config.h"

namespace mw::streamer::synchronizer {

enum class SynchronizerSinkState {
  kIdle,
  kRunning,
  kStandby,
  kDraining,
  kEnded,
  kFailed,
  kStopped,
};

// Real-time raw-frame scheduling on one owned thread. Submissions retain
// read-only buffer references and do not wait for output. Late/queued frames
// may be discarded. Audio has a continuous sample clock; video has a fixed
// frame-rate clock. Both clocks share a fixed max_frame_lateness playout
// buffer; source selection and output PTS do not move with frame arrivals.
// Missing audio becomes silence; video repeats briefly,
// then uses the standby image after standby_timeout without usable frames.
// Initial output requires one prototype from every declared track. Before
// that, queues stay bounded but unavailable formats cannot be synthesized.
//
// The first source generation establishes one downstream generation. Input
// interruption/reset/reconnect are absorbed so standby keeps encoders open;
// replacement tracks and frame formats must remain compatible. Within an
// input generation the source clock never reanchors to a late arrival. A new
// generation establishes a new source mapping while output PTS stay continuous.
// EOF drains retained media at playback pace and ends output without indefinite
// standby. Stop cancels pending work and joins before stopping every child.
//
// Invalid submissions throw synchronously. Worker errors are retained in
// error()/state(); FatalError and explicit child fatal reports additionally
// request Pipeline shutdown. Explicit message receivers must outlive senders.
// Lifecycle methods must not be called from callbacks or the owned thread.
class SynchronizerSink final : public sink::Sink {
 public:
  explicit SynchronizerSink(std::string id, SynchronizerSinkConfig config = {});
  ~SynchronizerSink() override;

  SynchronizerSink(const SynchronizerSink&) = delete;
  SynchronizerSink& operator=(const SynchronizerSink&) = delete;

  void OnStreamsReady(const media::FrameStreamsReady& streams) override;
  void OnAudioFrame(const media::FrameReady& frame) override;
  void OnVideoFrame(const media::FrameReady& frame) override;
  void OnTimelineReset(const media::TimelineReset& reset) override;
  void OnInputEnded(const media::StreamEnded& end) override;
  void Stop() noexcept override;

  SynchronizerSinkState state() const noexcept;
  std::string error() const;
  // Ingress plus scheduler-retained frames; excludes prototypes and in-flight
  // callbacks. Hardware/CPU media storage remains reference counted.
  std::size_t queue_depth() const;

 private:
  // kSynchronizer counts audio and video frame objects together. Its calls
  // cover scheduler Push/TakeReady, excluding pacing waits and all consumers.
  performance::NodeSnapshot GetOwnPerformance() const override;
  void HandleFatalError(const std::string& error) noexcept override;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::synchronizer

#endif  // MW_STREAMER_INCLUDE_MW_SYNCHRONIZER_SYNCHRONIZER_SINK_H_

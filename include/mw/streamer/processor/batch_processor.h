#ifndef MW_STREAMER_PROCESSOR_BATCH_PROCESSOR_H_
#define MW_STREAMER_PROCESSOR_BATCH_PROCESSOR_H_

#include <functional>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

// A synchronous decoded-frame consumer for batch processing. It owns no
// threads, queues, clock or device. Configure callbacks before Start and
// serialize processing and lifecycle calls. Input's batch frame callbacks may
// call ProcessVideo/ProcessAudio directly; processing time applies
// backpressure. All callbacks must consume their own exceptions, as with
// Processor.
class MW_STREAMER_API BatchProcessor final {
 public:
  // Receives the original input streams and the borrowed video device only for
  // this callback. Actual decoded properties and time_base belong to each
  // Frame; decoded timestamps retain the source stream's time_base. false
  // rejects the session.
  using OnReady = std::function<bool(const std::vector<ffmpeg::StreamInfo>&,
                                     const ffmpeg::HwDeviceContext&)>;
  // Frames are borrowed for the callback; Ref() retains their buffers. No frame
  // is returned downstream. Unset callbacks skip their channel. BatchProcessor
  // does not alter frame data, timestamps, format or audio sample_rate.
  using OnVideo = std::function<void(const ffmpeg::Frame&)>;
  using OnAudio = std::function<void(const ffmpeg::Frame&)>;
  using OnEnded = std::function<void()>;
  using OnStop = std::function<void()>;

  BatchProcessor() noexcept = default;
  ~BatchProcessor() noexcept;
  BatchProcessor(const BatchProcessor&) = delete;
  BatchProcessor& operator=(const BatchProcessor&) = delete;
  BatchProcessor(BatchProcessor&&) = delete;
  BatchProcessor& operator=(BatchProcessor&&) = delete;

  void SetOnReady(OnReady callback) noexcept;
  void SetOnVideo(OnVideo callback) noexcept;
  void SetOnAudio(OnAudio callback) noexcept;
  void SetOnEnded(OnEnded callback) noexcept;
  void SetOnStop(OnStop callback) noexcept;

  // Starts a session, or refreshes its input description after reopening the
  // source. Process frames only after Start succeeds and before End/Stop.
  // Callback configuration is retained across sessions.
  bool Start(const std::vector<ffmpeg::StreamInfo>& streams,
             const ffmpeg::HwDeviceContext& video_device) noexcept;
  void ProcessVideo(const ffmpeg::Frame& frame) noexcept;
  void ProcessAudio(const ffmpeg::Frame& frame) noexcept;
  // Call after the last input frame callback completes at normal EOF. End and
  // Stop each forward their event at most once for an accepted session.
  void End() noexcept;
  // Stop and join Input before calling Stop or destroying BatchProcessor.
  // Stop does not join external workers or synthesize an EOF event.
  void Stop() noexcept;

 private:
  OnReady on_ready_;
  OnVideo on_video_;
  OnAudio on_audio_;
  OnEnded on_ended_;
  OnStop on_stop_;
  bool started_ = false;
  bool ended_ = false;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PROCESSOR_BATCH_PROCESSOR_H_

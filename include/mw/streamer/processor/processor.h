#ifndef MW_STREAMER_PROCESSOR_PROCESSOR_H_
#define MW_STREAMER_PROCESSOR_PROCESSOR_H_

#include <functional>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

// A synchronous filter stage. It owns no threads, queues, clock or device.
// Configure callbacks before Start(). ProcessVideo and ProcessAudio may
// run concurrently; callbacks must consume their own exceptions. The external
// pipeline stops its Scheduler before calling Stop or destroying Processor.
class MW_STREAMER_API Processor final {
 public:
  // The device is borrowed only for this callback. false rejects the session.
  using OnReady = std::function<bool(const std::vector<ffmpeg::StreamInfo>&,
                                     const ffmpeg::HwDeviceContext&)>;
  // Return a frame for the encoder. Processor does not alter or validate its
  // format, dimensions, sample count or timestamps. Unset filters return Ref().
  using OnVideo = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;
  using OnAudio = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;
  using OnEnded = std::function<void()>;
  using OnStop = std::function<void()>;

  Processor() noexcept = default;
  ~Processor();
  Processor(const Processor&) = delete;
  Processor& operator=(const Processor&) = delete;
  Processor(Processor&&) = delete;
  Processor& operator=(Processor&&) = delete;

  void SetOnReady(OnReady callback) noexcept;
  void SetOnVideo(OnVideo callback) noexcept;
  void SetOnAudio(OnAudio callback) noexcept;
  void SetOnEnded(OnEnded callback) noexcept;
  void SetOnStop(OnStop callback) noexcept;

  // Once for a new session, after Stop. The pipeline processes frames only
  // after this succeeds; callback configuration is retained across sessions.
  bool Start(const std::vector<ffmpeg::StreamInfo>& streams,
             const ffmpeg::HwDeviceContext& video_device) noexcept;
  ffmpeg::Frame ProcessVideo(const ffmpeg::Frame& frame) noexcept;
  ffmpeg::Frame ProcessAudio(const ffmpeg::Frame& frame) noexcept;
  // The pipeline invokes End after Scheduler finishes all output callbacks.
  // End and Stop each forward their event at most once for an accepted session.
  void End() noexcept;
  // Call after upstream callbacks finish. Stop does not join external workers.
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

#endif  // MW_STREAMER_PROCESSOR_PROCESSOR_H_

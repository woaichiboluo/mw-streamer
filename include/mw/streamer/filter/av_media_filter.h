#ifndef MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_
#define MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_

#include <exception>
#include <functional>
#include <memory>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer {

// Synchronous, one-frame-in/one-frame-out business processing. Owns no source,
// queue, worker or media clock. The caller drives processing.
// EOF/underflow are passed through without running business callbacks.
//
// Process calls may run concurrently, including audio/video callbacks. The
// caller preserves per-track ordering and protects shared business state.
// Stop rejects new processing and waits for active callbacks before OnStop.
// Callbacks must not reenter this object. Finish all calls before destruction.
// Callback exceptions propagate to the caller and reject subsequent processing;
// already active calls may finish. Call Stop before starting another session.
class MW_STREAMER_API AvMediaFilter final {
 public:
  using OnStart = std::function<void()>;
  using OnStop = std::function<void()>;
  // Input headers and shared media buffers are read-only. Return an owned
  // frame; use writable/private storage when modifying pixels or samples.
  // Missing callbacks pass the original frame through. Callbacks change media
  // content only; the component preserves input timing on the returned header.
  using OnAudio = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;
  using OnVideo = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;

  AvMediaFilter();
  // Stops an active session; exceptions from OnStop are logged here.
  ~AvMediaFilter();
  AvMediaFilter(const AvMediaFilter&) = delete;
  AvMediaFilter& operator=(const AvMediaFilter&) = delete;
  AvMediaFilter(AvMediaFilter&&) = delete;
  AvMediaFilter& operator=(AvMediaFilter&&) = delete;

  // Configure only while stopped. Empty callbacks restore no-op/pass-through.
  void SetOnStart(OnStart callback);
  void SetOnStop(OnStop callback);
  void SetOnAudio(OnAudio callback);
  void SetOnVideo(OnVideo callback);

  // Calls OnStart once. Starting an active session throws std::logic_error.
  // If OnStart throws, the session remains stopped; the callback is responsible
  // for releasing resources allocated before its exception.
  void Start();
  // Waits for active processing and calls OnStop once. Idempotent, including
  // after an OnStop exception. Does not stop any upstream or downstream object.
  void Stop();

  // Thread-safe snapshot of the first processing callback failure. Stop keeps
  // it; a successful Start clears it. Start/Stop callback errors are excluded.
  std::exception_ptr error() const;

  // Require a started session. Consume the result's frame reference and return
  // the processed frame with its stream/time-base/EOF data intact.
  // Preserve PTS, DTS, best-effort timestamp, time base and duration exactly;
  // this is metadata forwarding, not timestamp repair or resynchronization.
  // EOF/underflow do not reset history or stop the session. Processing a frame
  // after a callback failure requires Stop/Start; it throws std::logic_error.
  AvDecoderReadResult ProcessVideo(AvDecoderReadResult input);
  AvDecoderReadResult ProcessAudio(AvDecoderReadResult input);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_

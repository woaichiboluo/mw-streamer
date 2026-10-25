#ifndef MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_
#define MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_

#include <exception>
#include <functional>
#include <memory>
#include <optional>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer {

// Synchronous business processing or frame consumption. Owns no source,
// queue, worker or media clock. The caller drives processing.
// EOF/underflow are passed through without running business callbacks.
//
// Process calls may run concurrently, including audio/video callbacks. The
// caller preserves per-track ordering and protects shared business state.
// Stop rejects new processing and waits for active callbacks before OnStop.
// Callbacks must not reenter this object. Finish all calls before destruction.
// Callback exceptions propagate to the caller and reject subsequent processing;
// already active calls may finish. Call Stop before starting another session.
// CUDA video callbacks in one session must use the same CUDA context. Its
// device is retained from the first CUDA video callback through OnStop.
// OnReset and OnStop run with that context current and restore the caller's
// context on return or throw. Before any CUDA video callback, they leave the
// caller's context unchanged. OnStart does not bind a CUDA context.
class MW_STREAMER_API AvMediaFilter final {
 public:
  using OnStart = std::function<void()>;
  using OnStop = std::function<void()>;
  using OnReset = std::function<void()>;
  // Input headers and shared media buffers are read-only. Return an owned
  // frame; use writable/private storage when modifying pixels or samples.
  // Missing callbacks pass the original frame through. Return std::nullopt
  // while buffering; no frame is forwarded. After buffering, return a frame
  // on every call. Callbacks own all output timing, including the time base;
  // this component never restores input timestamps on the returned frame.
  // CUDA video callbacks run with the input frame's CUDA context current on
  // the calling thread. The previous context is restored on return or throw.
  // Callbacks must restore any context stack changes they make themselves.
  using OnAudio =
      std::function<std::optional<ffmpeg::Frame>(const ffmpeg::Frame&)>;
  using OnVideo =
      std::function<std::optional<ffmpeg::Frame>(const ffmpeg::Frame&)>;

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
  void SetOnReset(OnReset callback);
  void SetOnAudio(OnAudio callback);
  void SetOnVideo(OnVideo callback);

  // Calls OnStart once. Starting an active session throws std::logic_error.
  // If OnStart throws, the session remains stopped; the callback is responsible
  // for releasing resources allocated before its exception.
  void Start();
  // Waits for active processing and calls OnStop once. Idempotent, including
  // after an OnStop exception. Does not stop any upstream or downstream object.
  void Stop();

  // Waits for active processing, then calls OnReset without ending the session.
  // The caller serializes Reset with Start/Stop/other Reset calls and pauses
  // new processing until it returns. EOF never invokes this automatically.
  // Requires a healthy started session. A callback exception is retained as
  // error() and rejects further processing until Stop/Start.
  void Reset();

  // Thread-safe snapshot of the first processing/reset callback failure. Stop
  // keeps it; a successful Start clears it. Start/Stop callback errors are
  // excluded.
  std::exception_ptr error() const;

  // Require a started session. Consume the result's frame reference and return
  // the processed frame with its stream/EOF data intact. The result time base
  // follows the returned frame's time base. All frame timing is forwarded
  // unchanged; no timestamp repair or resynchronization occurs here.
  // A std::nullopt callback result becomes kNull with no frame. A present,
  // moved-from Frame is invalid and fails the session.
  // EOF/underflow do not reset history or stop the session. Processing a frame
  // after a callback failure requires Stop/Start; it throws std::logic_error.
  AvDecoderReadResult ProcessVideo(AvDecoderReadResult input);
  AvDecoderReadResult ProcessAudio(AvDecoderReadResult input);

  // Execute the same business callbacks but discard their output. Empty or
  // invalid returned frames are accepted.
  // Session, input validation, EOF and failure semantics match Process calls.
  void ConsumeVideo(AvDecoderReadResult input);
  void ConsumeAudio(AvDecoderReadResult input);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_FILTER_AV_MEDIA_FILTER_H_

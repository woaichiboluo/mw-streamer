#ifndef MW_STREAMER_SINK_FRAME_CUSTOM_SINK_H_
#define MW_STREAMER_SINK_FRAME_CUSTOM_SINK_H_

#include "mw/streamer/processor/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef MwStreamerProcessorStartResult (
    *MwStreamerFrameCustomSinkStartCallback)(
    const MwStreamerProcessorSourceInfo* source_info, void* user_context);

typedef void (*MwStreamerFrameCustomSinkVideoCallback)(
    const MwStreamerVideoFrameView* frame, void* user_context);
typedef void (*MwStreamerFrameCustomSinkAudioCallback)(
    const MwStreamerAudioFrameView* frame, void* user_context);

typedef struct MwStreamerFrameCustomSinkCallbacks {
  // Borrowed user data returned unchanged to every callback. The framework
  // never reads or releases it.
  void* user_context;

  // Called at most once. Source information stays fixed for the Sink lifetime;
  // a reconnect that changes it is rejected instead of calling on_start
  // again. Source information is borrowed for the callback.
  MwStreamerFrameCustomSinkStartCallback on_start;

  // Synchronously consume borrowed decoded frames. Audio and video callbacks
  // may execute concurrently; each track remains ordered.
  MwStreamerFrameCustomSinkVideoCallback on_frame;
  MwStreamerFrameCustomSinkAudioCallback on_audio;

  // Optional. Called once per input boundary after in-flight media and message
  // callbacks finish. EOF does not stop the sink or invoke on_stop.
  MwStreamerProcessorBoundaryCallback on_boundary;

  // Optional. Called once on Stop after successful initialization and after
  // in-flight callbacks finish. Exceptions are logged and suppressed. If
  // on_start is omitted, initialization is considered successful.
  MwStreamerProcessorStopCallback on_stop;

  // Optional. Pipeline delivers messages asynchronously after initialization.
  // Message callbacks are serialized on its message Poller and may overlap
  // audio/video callbacks, but not lifecycle boundaries. All message data is
  // borrowed for the callback. Stop waits for in-flight callbacks. A callback
  // may call Pipeline::SubmitMessage, but must not stop or destroy the sink or
  // Pipeline. If null, messages are ignored.
  MwStreamerProcessorMessageCallback on_message;
} MwStreamerFrameCustomSinkCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_SINK_FRAME_CUSTOM_SINK_H_

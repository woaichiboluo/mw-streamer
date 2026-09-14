#ifndef MW_STREAMER_SINK_CUSTOM_SINK_H_
#define MW_STREAMER_SINK_CUSTOM_SINK_H_

#include <stddef.h>

#include "mw/streamer/processor/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MwStreamerSendMessageCallback)(
    void* context, const char* type, const void* payload, size_t payload_size,
    const MwStreamerMediaTimestamp* timestamp);

// Copy this value during on_start when messages need to be sent later. The
// copied sender is valid until the owning Pipeline is destroyed; sends during
// or after Stop are ignored. type and payload are borrowed only for send.
typedef struct MwStreamerMessageSender {
  void* context;
  MwStreamerSendMessageCallback send;
} MwStreamerMessageSender;

typedef MwStreamerProcessorStartResult (*MwStreamerCustomSinkStartCallback)(
    const MwStreamerProcessorSourceInfo* source_info,
    const MwStreamerMessageSender* message_sender, void* user_context);

typedef void (*MwStreamerCustomSinkVideoCallback)(
    const MwStreamerVideoFrameView* frame, void* user_context);
typedef void (*MwStreamerCustomSinkAudioCallback)(
    const MwStreamerAudioFrameView* frame, void* user_context);

typedef struct MwStreamerCustomSinkCallbacks {
  // Borrowed user data returned unchanged to every callback. The framework
  // never reads or releases it.
  void* user_context;

  // Called at most once. Source information stays fixed for the Sink lifetime;
  // a reconnect that changes it fails the Pipeline instead of calling on_start
  // again. Both arguments are borrowed for the callback; message_sender may be
  // copied by value for later use.
  MwStreamerCustomSinkStartCallback on_start;

  // Synchronously consume borrowed decoded frames. Audio and video callbacks
  // may execute concurrently; each track remains ordered.
  MwStreamerCustomSinkVideoCallback on_frame;
  MwStreamerCustomSinkAudioCallback on_audio;
} MwStreamerCustomSinkCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_SINK_CUSTOM_SINK_H_

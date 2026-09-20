#ifndef MW_STREAMER_SINK_PACKET_CUSTOM_SINK_H_
#define MW_STREAMER_SINK_PACKET_CUSTOM_SINK_H_

#include "mw/streamer/processor/processor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef MwStreamerProcessorStartResult (
    *MwStreamerPacketCustomSinkStartCallback)(
    const MwStreamerProcessorSourceInfo* source_info, void* user_context);

// packet points to a const AVPacket. The packet and its referenced data are
// read-only and borrowed for this callback only. Do not free or modify them.
// Retaining a packet for asynchronous work requires taking an independent
// reference during the callback (for example av_packet_clone) and releasing
// that reference afterward. This header does not require FFmpeg headers.
typedef void (*MwStreamerPacketCustomSinkPacketCallback)(const void* packet,
                                                         void* user_context);

typedef struct MwStreamerPacketCustomSinkCallbacks {
  // Borrowed until Stop completes; the framework never releases this pointer.
  void* user_context;

  // Optional, called at most once before packets/messages. Source information
  // is borrowed for this call and stays fixed across reconnects. A changed
  // source fails the Pipeline. A null callback accepts startup. A failed
  // startup does not receive on_stop.
  MwStreamerPacketCustomSinkStartCallback on_start;

  // Optional, synchronous on the producer's execution context. Audio and video
  // packets are serialized in producer order. Slow callbacks block upstream.
  MwStreamerPacketCustomSinkPacketCallback on_video_packet;
  MwStreamerPacketCustomSinkPacketCallback on_audio_packet;

  // Optional. One notification per accepted timeline reset or normal EOF,
  // after preceding packet callbacks. EOF does not stop this sink.
  MwStreamerProcessorBoundaryCallback on_boundary;

  // Optional, called once on actual Stop after successful startup, with no
  // media/message callbacks in flight. Exceptions are logged and suppressed.
  MwStreamerProcessorStopCallback on_stop;

  // Optional. Pipeline serializes messages on its message Poller. May overlap
  // packet callbacks, but not start/boundary/stop. All message data is borrowed
  // for the callback. Callbacks may send messages, but must not stop or destroy
  // the sink or Pipeline. Unset callbacks ignore messages.
  MwStreamerProcessorMessageCallback on_message;
} MwStreamerPacketCustomSinkCallbacks;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_SINK_PACKET_CUSTOM_SINK_H_

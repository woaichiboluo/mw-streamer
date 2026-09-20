#ifndef MW_STREAMER_SINK_SINK_MESSAGE_H_
#define MW_STREAMER_SINK_SINK_MESSAGE_H_

#include <stddef.h>
#include <stdint.h>

#include "mw/streamer/media/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Zero-initialize before filling fields. type must be a non-null,
// null-terminated string; payload may be null only when payload_size is zero.
// Pipeline::SubmitMessage copies type and payload bytes before returning and
// releases its internal copy automatically. Pointers embedded in payload are
// not followed or copied. The caller retains ownership of the original data.
// Receivers borrow all fields for one callback and must not free them; copy
// any data needed after returning. timestamp is used only when has_timestamp
// is nonzero and does not impose ordering relative to media delivery.
typedef struct MwStreamerMessage {
  const char* type;
  const void* payload;
  size_t payload_size;
  uint8_t has_timestamp;
  MwStreamerMediaTimestamp timestamp;
} MwStreamerMessage;

#ifdef __cplusplus
}
#endif

#endif  // MW_STREAMER_SINK_SINK_MESSAGE_H_

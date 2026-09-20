#include "mw/streamer/sink/sink_message.h"

MwStreamerMessage MakeCMessage(uint8_t has_timestamp) {
  static const char payload[] = {'c', '\0', 'x'};
  MwStreamerMessage message = {0};
  message.type = "c-message";
  message.payload = payload;
  message.payload_size = sizeof(payload);
  message.has_timestamp = has_timestamp;
  message.timestamp.pts = 123;
  message.timestamp.duration = 7;
  message.timestamp.time_base.num = 1;
  message.timestamp.time_base.den = 90000;
  return message;
}

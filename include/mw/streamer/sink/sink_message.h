#ifndef MW_STREAMER_SINK_SINK_MESSAGE_H_
#define MW_STREAMER_SINK_SINK_MESSAGE_H_

#include <cstddef>
#include <optional>
#include <string_view>

#include "mw/streamer/media/types.h"

namespace mw::streamer {

// Borrowed for one submission. Receivers must copy strings and payload before
// returning if processing asynchronously. The sender supplies sink_id;
// timestamp is informational and does not impose ordering relative to media
// delivery.
struct SinkMessage {
  std::string_view sink_id;
  std::string_view type;
  const void* payload = nullptr;
  std::size_t payload_size = 0;
  std::optional<MwStreamerMediaTimestamp> timestamp;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_SINK_SINK_MESSAGE_H_

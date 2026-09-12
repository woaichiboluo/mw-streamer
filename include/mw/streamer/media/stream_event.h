#ifndef MW_STREAMER_MEDIA_STREAM_EVENT_H_
#define MW_STREAMER_MEDIA_STREAM_EVENT_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hardware_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

enum class StreamEndReason { kEof, kInterrupted, kStopped, kFailed };
enum class TimelineResetReason { kReconnect, kSeek };

// Offline sources may block in sink delivery and require complete processing.
// Real-time sources keep their existing bounded, dropping queue policy.
enum class StreamDeliveryMode { kRealtime, kOffline };

// Arguments own their metadata and packet references. Copy them to retain them
// after synchronous delivery; packet copies share read-only media buffers.
struct StreamsReady {
  std::uint64_t generation;
  std::vector<StreamInfo> streams;
  StreamDeliveryMode delivery_mode = StreamDeliveryMode::kRealtime;
};

struct PacketReady {
  std::uint64_t generation;
  Packet packet;
};

struct TimelineReset {
  std::uint64_t generation;
  TimelineResetReason reason;
  std::optional<std::chrono::milliseconds> position;
};

// Ends one generation that had StreamsReady. Reconnection may open another.
// Failure before streams are ready produces only InputStateChanged.
struct StreamEnded {
  std::uint64_t generation;
  StreamEndReason reason;
};

struct FrameStreamsReady {
  std::uint64_t generation;
  std::vector<StreamInfo> source_streams;
  // Borrowed until downstream Stop completes. Null for software frames.
  const HardwareContext* hardware_context = nullptr;
};

struct FrameReady {
  std::uint64_t generation;
  Frame frame;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_MEDIA_STREAM_EVENT_H_

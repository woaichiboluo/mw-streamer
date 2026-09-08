#ifndef MW_STREAMER_INCLUDE_MW_SINK_PACKET_SINK_H_
#define MW_STREAMER_INCLUDE_MW_SINK_PACKET_SINK_H_

#include <cstdint>
#include <vector>

#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::sink {

// Consumes compressed audio/video, whether received from an input or produced
// by an encoder. Calls are synchronous and serialized by the producer.
// Queueing, worker threads, overload handling, and error reporting belong to
// the Sink. Implementations handle their own failures without throwing into the
// producer.
class PacketSink {
 public:
  virtual ~PacketSink() = default;

  // Precedes packets for this generation. A newer generation replaces the old
  // timeline, including after a seek; the replaced timeline need not receive
  // EndInput. Streams are borrowed for this call and must be copied to retain.
  virtual void SetStreams(
      std::uint64_t generation,
      const std::vector<ffmpeg::StreamInfo>& streams) noexcept = 0;

  // Borrows a read-only packet for this call. Copy or Ref it before returning
  // to retain it for asynchronous work. Referenced buffers remain read-only
  // because other Sinks may share them. Returning does not imply processing
  // completed; blocking here blocks the producer and subsequent Sinks.
  virtual void Write(std::uint64_t generation,
                     const ffmpeg::Packet& packet) noexcept = 0;

  // No more packets will arrive for this generation. This can follow EOF,
  // interruption, or explicit input stop; a later SetStreams may start another
  // generation. This notification does not require synchronous draining or
  // destruction. The Sink owner controls completion and resource shutdown.
  virtual void EndInput(std::uint64_t generation) noexcept = 0;
};

}  // namespace mw::streamer::sink

#endif  // MW_STREAMER_INCLUDE_MW_SINK_PACKET_SINK_H_

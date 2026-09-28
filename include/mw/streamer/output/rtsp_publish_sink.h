#ifndef MW_STREAMER_OUTPUT_RTSP_PUBLISH_SINK_H_
#define MW_STREAMER_OUTPUT_RTSP_PUBLISH_SINK_H_

#include <cstddef>
#include <memory>
#include <string>

#include "mw/streamer/output/config.h"
#include "mw/streamer/performance/pipeline_snapshot.h"
#include "mw/streamer/sink/sink.h"

namespace mw::streamer {

// Publishes one encoded stream at app/stream for RTSP clients to pull. The
// listener starts when the first publisher becomes ready and closes after the
// last publisher ends or stops. Multiple publishers on one endpoint share the
// listener; each path is unique process-wide. This sink owns a bounded packet
// queue consumed on a ZLToolKit Poller. H264/H265 packets must use Annex B.
// Reconnects retain the media source while DTS remains nondecreasing; a DTS
// regression rebuilds it and can disconnect existing RTSP clients. EOF drains
// accepted packets and unregisters the source. Interruption keeps it alive.
// Failure affects only this sink; FatalError also requests Pipeline shutdown.
class RtspPublishSink final : public Sink {
 public:
  explicit RtspPublishSink(std::string id, RtspPublishSinkConfig config);
  ~RtspPublishSink() override;

  RtspPublishSink(const RtspPublishSink&) = delete;
  RtspPublishSink& operator=(const RtspPublishSink&) = delete;

  void OnStreamsReady(const StreamsReady& streams) noexcept override;
  void OnPacket(const PacketReady& packet) noexcept override;
  void OnTimelineReset(const TimelineReset& reset) noexcept override;
  void OnInputEnded(const StreamEnded& end) noexcept override;

  // Stop drains accepted work and unregisters the stream. Call after upstream
  // delivery stops and outside this sink's Poller. No callback retains this
  // sink after Stop returns. A failed state remains failed.
  void Stop() noexcept override;
  PacketSinkState state() const noexcept;
  std::string error() const;
  std::size_t queue_depth() const;

 private:
  NodeSnapshot GetOwnPerformance() const override;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_OUTPUT_RTSP_PUBLISH_SINK_H_

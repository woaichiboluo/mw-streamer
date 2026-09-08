#ifndef MW_STREAMER_INCLUDE_MW_OUTPUT_REMUX_SINK_H_
#define MW_STREAMER_INCLUDE_MW_OUTPUT_REMUX_SINK_H_

#include <cstddef>
#include <memory>
#include <string>

#include "mw/output/config.h"
#include "mw/performance/pipeline_snapshot.h"
#include "mw/sink/sink.h"
#include "mw/zlm/config.h"

namespace mw::streamer::output {

// Copies packet references into its own bounded queue, consumed on one Poller
// selected internally from ZLToolKit's pool. No additional worker is required.
// One sink owns one output target; it neither decodes nor encodes. H264/H265
// packets must use Annex B, as provided by ZlmInput and the project encoders.
// Source parameters stay stable across generations. Reconnects preserve the
// output while DTS remains nondecreasing per track. A DTS regression rebuilds
// the output (a new recording file or publisher). Packets are never filtered
// while waiting for a keyframe. Recording preserves encoded samples and their
// timestamp intervals (at millisecond resolution), using one common AV origin.
// Startup packets wait for each track's first packet and codec parameters;
// EOF also writes a partial set of tracks if their codec parameters are known.
// An unconvertible packet or a full startup cache fails this sink explicitly.
// Network delivery retains ZLM's live GOP cache and reconnection semantics;
// it does not guarantee replay of all packets submitted before a connection.
// PTS reordering is allowed. Temporary network failures retain ZLM retry;
// permanent target errors and queue overflow fail only this sink. Explicit
// FatalError additionally requests Pipeline shutdown through Sink.
class RemuxSink final : public sink::Sink {
 public:
  explicit RemuxSink(std::string id, RemuxSinkConfig config);
  ~RemuxSink() override;

  RemuxSink(const RemuxSink&) = delete;
  RemuxSink& operator=(const RemuxSink&) = delete;

  void OnStreamsReady(const media::StreamsReady& streams) noexcept override;
  void OnPacket(const media::PacketReady& packet) noexcept override;
  void OnTimelineReset(const media::TimelineReset& reset) noexcept override;
  void OnInputEnded(const media::StreamEnded& end) noexcept override;

  // EOF drains and finalizes to kEnded; interruption waits for another
  // generation. Explicit Stop drains accepted work and finalizes to kStopped,
  // preserving kFailed. Stop/destruction must follow upstream delivery and
  // run outside this sink's Poller. No callbacks retain this sink afterwards.
  void Stop() noexcept override;
  sink::PacketSinkState state() const noexcept;
  std::string error() const;
  std::size_t queue_depth() const;
  // Thread-safe snapshot for the single target. Recording has no network
  // traffic. This query is not a media/control callback and may block briefly.
  performance::NetworkOutputSnapshot GetNetworkOutputSnapshot() const;

 private:
  // Calls measure local packet conversion and muxing, including startup cache
  // flushes at EOF. Startup metadata discovery and network delivery are not
  // timed. Reading never waits for the output Poller.
  performance::NodeSnapshot GetOwnPerformance() const override;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::output

#endif  // MW_STREAMER_INCLUDE_MW_OUTPUT_REMUX_SINK_H_

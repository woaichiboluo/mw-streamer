#ifndef MW_STREAMER_INCLUDE_MW_INPUT_INTERNAL_ZLM_TIMESTAMP_REVISER_H_
#define MW_STREAMER_INCLUDE_MW_INPUT_INTERNAL_ZLM_TIMESTAMP_REVISER_H_

#include <memory>
#include <string_view>

namespace mediakit {
class Frame;
class Stamp;
}  // namespace mediakit

namespace mw::streamer::input::internal {

// ZLM's RTSP player reconstructs each RTP track against its own RTCP/NTP
// clock. Its MultiMediaSourceMuxer normally smooths the resulting jumps with
// Stamp before forwarding decoded frames. PlayerProxy bypasses that muxer, so
// it applies the same policy at the AVPacket input boundary.
bool ShouldReviseZlmTimestamps(std::string_view url) noexcept;

// Mirrors MultiMediaSourceMuxer::onTrackFrame(). All timestamp calculations
// remain owned by ZLM's Stamp and FrameStamp implementations.
std::shared_ptr<mediakit::Frame> ReviseZlmFrameTimestamp(
    std::shared_ptr<mediakit::Frame> frame, mediakit::Stamp* stamp);

}  // namespace mw::streamer::input::internal

#endif  // MW_STREAMER_INCLUDE_MW_INPUT_INTERNAL_ZLM_TIMESTAMP_REVISER_H_

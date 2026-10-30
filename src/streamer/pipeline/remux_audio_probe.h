#ifndef MW_SRC_STREAMER_PIPELINE_REMUX_AUDIO_PROBE_H_
#define MW_SRC_STREAMER_PIPELINE_REMUX_AUDIO_PROBE_H_

#include <vector>

#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer::internal {

// Completes one selected AAC track from at most eight retained audio packets.
// Uses actual ASC/ADTS configuration first. Otherwise verifies an LC-compatible
// description against the same short sample decoded without ASC; this does not
// recover the original profile or validate the rest of the stream. The caller
// assumes stable encoding, retains/replays the original packets, and owns input
// selection, cancellation, and buffering. No packet or input metadata is
// changed.
ffmpeg::StreamInfo ProbeRemuxAudio(const ffmpeg::StreamInfo& stream,
                                   const std::vector<ffmpeg::Packet>& packets);

}  // namespace mw::streamer::internal

#endif  // MW_SRC_STREAMER_PIPELINE_REMUX_AUDIO_PROBE_H_

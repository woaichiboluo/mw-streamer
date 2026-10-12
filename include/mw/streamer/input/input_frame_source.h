#ifndef MW_STREAMER_INPUT_INPUT_FRAME_SOURCE_H_
#define MW_STREAMER_INPUT_INPUT_FRAME_SOURCE_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace mw::streamer {

// A bounded decoder queue, pulled when its downstream filter needs input.
// Exactly one consumer per track. Retaining this value retains the queue;
// try_receive is nonblocking and rethrows decoder errors after valid output.
struct InputFrameSource {
  ffmpeg::StreamInfo stream;
  std::function<bool(ffmpeg::Frame&)> try_receive;
  std::function<bool()> finished;
  std::function<std::int64_t()> end_pts;
};

// Tracks of an opened segment retain native timestamps. Source positions use
// AV_TIME_BASE units; reopening and seeking create new decoder queues.
struct DecodedInputSource {
  std::vector<InputFrameSource> tracks;
  // Absolute filter start position, including a requested seek offset.
  std::int64_t start_time_us = 0;
  // Container origin, independent of seeking. Unknown container origin is zero.
  std::int64_t media_origin_us = 0;
  // Reopen after producing more than two channel frames at the segment end.
  bool loop = false;
  // An explicit seek discards pending old segments and produced frames.
  // A normal loop retains already produced frames.
  bool flush = false;
  // Remaining container duration for this segment, in AV_TIME_BASE units.
  // Unknown duration does not impose a producer boundary.
  std::int64_t duration_us = AV_NOPTS_VALUE;
  // A filter consumer completes the segment at filter EOF or its duration
  // boundary. Report actual produced channel frames, not raw decoded frames.
  // Input sources require this signal; standalone synthetic sources may omit
  // it. Capturing this function does not retain or access the Input object.
  std::function<void(std::int64_t produced_frames)> complete = {};
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_INPUT_INPUT_FRAME_SOURCE_H_

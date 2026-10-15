#ifndef MW_STREAMER_FILTER_FORCE_FPS_H_
#define MW_STREAMER_FILTER_FORCE_FPS_H_

#include <cstddef>
#include <memory>

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"

namespace mw::streamer {

struct ForceFpsConfig {
  AVRational fps{25, 1};
  // Maximum queued inputs, including EOF and the frame currently being
  // expanded. Must be positive. No separate output queue is allocated.
  std::size_t input_capacity = 8;
};

// Pull-driven video frame-rate conversion. No worker, wall-clock pacing,
// interpolation or backup picture generation. One producer and one reader may
// run concurrently. Finish all calls before destruction.
//
// Copies frame references, never pixels or CUDA surfaces. Output headers own
// their timing independently of inputs and previously returned frames. Duration
// is one tick in inverse-fps time base; best_effort_timestamp follows output
// PTS and pkt_dts is unset because a converted frame has no packet decode time.
// Shared media buffers must remain read-only while queued or retained by
// output.
class MW_STREAMER_API ForceFps final {
 public:
  // Invalid frame rate or capacity throws std::invalid_argument.
  explicit ForceFps(ForceFpsConfig config = {});
  ~ForceFps();
  ForceFps(const ForceFps&) = delete;
  ForceFps& operator=(const ForceFps&) = delete;
  ForceFps(ForceFps&&) = delete;
  ForceFps& operator=(ForceFps&&) = delete;

  // Returns false when full or stopped, without consuming input or changing
  // admission state. Retain rejected input and retry after reading.
  // Accepts video frames with a known source PTS and valid
  // result time_base, or EOF. Invalid input throws. Input after EOF is accepted
  // in queue order; consuming EOF clears conversion history.
  bool TryPushFrame(const AvDecoderReadResult& input);
  // Produces at most one output per call, discarding surplus input as needed.
  // Gaps repeat the most recent picture lazily, leaving the current input at
  // the queue head until reached. No input returns kNull, even with a cached
  // picture. EOF follows prior outputs, is rescaled to output time base and
  // does not extend the final picture. A PTS regression or a forward step
  // exceeding one output period plus 500ms reanchors without filling that jump,
  // missing PTS is never synthesized.
  AvDecoderReadResult TryReadFrame();
  // Closes admission, discards pending input and makes reads
  // return kNull. Does not wait for producer or reader calls to finish.
  void Stop();
  // Discards queued inputs and conversion history, reopening admission.
  // To reuse an active session, first Stop(),
  // finish all producer/reader calls, then Reset(); Reset must never overlap
  // producers from the previous session.
  void Reset();
  AVRational time_base() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_FILTER_FORCE_FPS_H_

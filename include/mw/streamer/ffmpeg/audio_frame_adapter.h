#ifndef MW_STREAMER_FFMPEG_AUDIO_FRAME_ADAPTER_H_
#define MW_STREAMER_FFMPEG_AUDIO_FRAME_ADAPTER_H_

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/filter_graph.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::streamer::ffmpeg::internal {

// Serial audio format conversion and reblocking through a filter graph. Input
// and output share a sample rate and channel layout. A positive frame_size
// requests that many samples per channel; the final frame at End() follows
// FFmpeg's sink EOF behavior. Zero preserves variable output sizes. Submit
// retains the caller's frame and timeline; Receive uses the time base
// 1/sample_rate.
class AudioFrameAdapter final {
 public:
  AudioFrameAdapter(AVSampleFormat input, AVSampleFormat output,
                    int sample_rate, const AVChannelLayout& layout,
                    int frame_size);
  AudioFrameAdapter(const AudioFrameAdapter&) = delete;
  AudioFrameAdapter& operator=(const AudioFrameAdapter&) = delete;

  // Requires valid PTS, a positive time base and nonnegative duration. Submit
  // before End(), and drain Receive() after each submission and after End().
  void Submit(const Frame& frame);
  void End();
  // False means more input is needed, or the ended graph has been drained.
  bool Receive(Frame& frame);

 private:
  const AVRational time_base_;
  FilterGraph graph_;
  AVFilterContext* source_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg::internal

#endif  // MW_STREAMER_FFMPEG_AUDIO_FRAME_ADAPTER_H_

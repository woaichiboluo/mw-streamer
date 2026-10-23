#ifndef MW_STREAMER_FILTER_AUDIO_RESAMPLER_H_
#define MW_STREAMER_FILTER_AUDIO_RESAMPLER_H_

#include <cstddef>
#include <memory>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include "mw/export.h"
#include "mw/streamer/decoder/av_decoder.h"

namespace mw::streamer {

struct AudioResamplerConfig {
  int sample_rate = 48000;
  AVSampleFormat sample_format = AV_SAMPLE_FMT_FLTP;
  // Borrowed during construction, then copied independently.
  AVChannelLayout channel_layout = AV_CHANNEL_LAYOUT_STEREO;
  // Maximum queued inputs, including EOF. Must be positive.
  std::size_t input_capacity = 8;
  // Samples per channel in each output frame; zero preserves variable sizes.
  // Positive sizes pad boundary/EOF tails with silence. EOF end_pts remains
  // the actual source endpoint. Negative sizes are invalid.
  int output_samples_per_frame = 0;
};

// Pull-driven audio conversion before Syncer. No worker, wall clock, gap
// filling or async timestamp compensation. Fixed-size tails may be padded.
// Copies input references into a bounded queue; conversion and delay draining
// happen in TryReadFrame(). One producer and one reader may run concurrently;
// finish calls before destruction.
class MW_STREAMER_API AudioResampler final {
 public:
  // Invalid audio target/capacity throws synchronously. Capacity counts queued
  // inputs including EOF; there is no separate output queue.
  explicit AudioResampler(AudioResamplerConfig config = {});
  ~AudioResampler();
  AudioResampler(const AudioResampler&) = delete;
  AudioResampler& operator=(const AudioResampler&) = delete;
  AudioResampler(AudioResampler&&) = delete;
  AudioResampler& operator=(AudioResampler&&) = delete;

  // Returns false when full, stopped or failed, without consuming input or
  // changing admission state. Retain rejected input and retry after reading.
  // Requires CPU audio, valid source PTS/time_base and a valid actual sample
  // format/rate/layout. Invalid input throws. Input after EOF is accepted.
  // Media buffers must remain read-only.
  bool TryPushFrame(const AvDecoderReadResult& input);
  // Returns at most one frame or EOF, or kNull when no output is available.
  // Output uses 1/target_sample_rate, duration=nb_samples and delay-compensated
  // source PTS. Input PTS jumps remain visible; this is not a synchronizer.
  // Format/stream changes drain the old converter before processing
  // the new input. EOF drains all delayed samples first, then forwards the
  // source end_pts in the target time base. With output_samples_per_frame > 0,
  // reblocks converted audio before delivery. Timestamp quantization within the
  // source time-base precision follows the sample clock; larger PTS
  // discontinuities and stream/format changes drain the old block before the
  // new segment. Conversion failures throw and are retained until Reset; later
  // reads rethrow and further submissions return false. Invalid submissions do
  // not poison the converter.
  AvDecoderReadResult TryReadFrame();
  // Closes admission, discards queued inputs and conversion delay. Finish old
  // producer/reader calls before Reset or destruction; Stop does not join them.
  void Stop();
  void Reset();
  AVRational time_base() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_FILTER_AUDIO_RESAMPLER_H_

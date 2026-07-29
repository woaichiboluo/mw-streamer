#ifndef MW_STREAMER_INCLUDE_MW_RESAMPLER_AUDIO_RESAMPLER_H_
#define MW_STREAMER_INCLUDE_MW_RESAMPLER_AUDIO_RESAMPLER_H_

#include <functional>
#include <memory>

#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/stream_info.h"

namespace mw::streamer::resampler {

class AudioResampler final {
 public:
  static constexpr int kOutputSampleRate = 48000;

  using OnFrame = std::function<void(const ffmpeg::Frame& frame)>;

  explicit AudioResampler(ffmpeg::StreamInfo stream_info);
  ~AudioResampler();

  AudioResampler(const AudioResampler&) = delete;
  AudioResampler& operator=(const AudioResampler&) = delete;

  // Resampling and callbacks are synchronous on the calling thread. Output is
  // always 48 kHz interleaved float32 with the source channel layout. The frame
  // is borrowed for OnFrame; copy or call Ref to retain it.
  void SetOnFrame(OnFrame callback);
  void Resample(const ffmpeg::Frame& frame);

  // Drain emits delayed samples and ends the current timeline. Resample cannot
  // be called again until Flush discards the old state and starts a new one.
  void Drain();
  void Flush();

  const ffmpeg::StreamInfo& stream_info() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::resampler

#endif  // MW_STREAMER_INCLUDE_MW_RESAMPLER_AUDIO_RESAMPLER_H_

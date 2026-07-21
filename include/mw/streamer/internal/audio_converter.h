#ifndef MW_STREAMER_INTERNAL_AUDIO_CONVERTER_H_
#define MW_STREAMER_INTERNAL_AUDIO_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

inline constexpr int kProcessorAudioSampleRate = 48'000;
inline constexpr int kProcessorAudioChannelCount = 2;
inline constexpr std::size_t kProcessorAudioBlockFrameCount = 1'024;

struct NormalizedAudioFrame {
  std::vector<float> samples;
  std::size_t frame_count = 0;
  int64_t pts = MW_TIMESTAMP_UNAVAILABLE;
  MwRational time_base{0, 1};
  int64_t pts_us = MW_TIMESTAMP_UNAVAILABLE;

  [[nodiscard]] MwAudioFrameView view() noexcept;
};

class AudioConverter {
 public:
  AudioConverter();
  ~AudioConverter();

  AudioConverter(const AudioConverter&) = delete;
  AudioConverter& operator=(const AudioConverter&) = delete;
  AudioConverter(AudioConverter&&) = delete;
  AudioConverter& operator=(AudioConverter&&) = delete;

  [[nodiscard]] std::optional<NormalizedAudioFrame> Convert(
      const ffmpeg::Frame& input);
  [[nodiscard]] std::optional<NormalizedAudioFrame> Flush();

  [[nodiscard]] bool is_configured() const noexcept;

 private:
  struct State;

  void Configure(const ffmpeg::Frame& input);
  void ValidateInput(const ffmpeg::Frame& input) const;
  [[nodiscard]] std::optional<NormalizedAudioFrame> ConvertSamples(
      const uint8_t* const* input_data, int input_frame_count);
  void SetOutputTimestamp(NormalizedAudioFrame* output) const;
  void AnchorTimeline(const ffmpeg::Frame& input);

  std::unique_ptr<State> state_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_AUDIO_CONVERTER_H_

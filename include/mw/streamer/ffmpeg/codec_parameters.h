#ifndef MW_STREAMER_FFMPEG_CODEC_PARAMETERS_H_
#define MW_STREAMER_FFMPEG_CODEC_PARAMETERS_H_

extern "C" {
#include <libavcodec/codec_par.h>
}

namespace mw::streamer::ffmpeg {

class CodecParameters final {
 public:
  CodecParameters();
  ~CodecParameters() noexcept;

  CodecParameters(const CodecParameters&) = delete;
  CodecParameters& operator=(const CodecParameters&) = delete;
  CodecParameters(CodecParameters&& other) noexcept;
  CodecParameters& operator=(CodecParameters&& other) noexcept;

  [[nodiscard]] AVCodecParameters* get() noexcept;
  [[nodiscard]] const AVCodecParameters* get() const noexcept;
  [[nodiscard]] CodecParameters Clone() const;

 private:
  AVCodecParameters* parameters_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_CODEC_PARAMETERS_H_

#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_PARAMETERS_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_PARAMETERS_H_

extern "C" {
#include <libavcodec/codec_par.h>
}

namespace mw::streamer::ffmpeg {

class CodecParameters final {
 public:
  CodecParameters();
  explicit CodecParameters(const AVCodecParameters& source);
  ~CodecParameters();

  CodecParameters(const CodecParameters& other);
  CodecParameters& operator=(const CodecParameters& other);
  CodecParameters(CodecParameters&& other) noexcept;
  CodecParameters& operator=(CodecParameters&& other) noexcept;

  const AVCodecParameters* get() const noexcept;
  AVCodecParameters* get() noexcept;

  void Swap(CodecParameters& other) noexcept;

 private:
  AVCodecParameters* parameters_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_PARAMETERS_H_

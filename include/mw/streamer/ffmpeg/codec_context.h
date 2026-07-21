#ifndef MW_STREAMER_FFMPEG_CODEC_CONTEXT_H_
#define MW_STREAMER_FFMPEG_CODEC_CONTEXT_H_

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace mw::streamer::ffmpeg {

class CodecContext final {
 public:
  explicit CodecContext(const AVCodec* codec);
  ~CodecContext() noexcept;

  CodecContext(const CodecContext&) = delete;
  CodecContext& operator=(const CodecContext&) = delete;
  CodecContext(CodecContext&& other) noexcept;
  CodecContext& operator=(CodecContext&& other) noexcept;

  [[nodiscard]] AVCodecContext* get() noexcept;
  [[nodiscard]] const AVCodecContext* get() const noexcept;

 private:
  AVCodecContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_CODEC_CONTEXT_H_

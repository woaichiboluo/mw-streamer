#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_CONTEXT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_CONTEXT_H_

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace mw::streamer::ffmpeg {

class CodecContext final {
 public:
  explicit CodecContext(const AVCodec* codec);
  ~CodecContext();

  CodecContext(const CodecContext&) = delete;
  CodecContext& operator=(const CodecContext&) = delete;
  CodecContext(CodecContext&& other) noexcept;
  CodecContext& operator=(CodecContext&& other) noexcept;

  const AVCodecContext* get() const noexcept;
  AVCodecContext* get() noexcept;
  void FlushBuffers() noexcept;

 private:
  AVCodecContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_CODEC_CONTEXT_H_

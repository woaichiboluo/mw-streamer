#ifndef MW_STREAMER_FFMPEG_BUFFER_REF_H_
#define MW_STREAMER_FFMPEG_BUFFER_REF_H_

extern "C" {
#include <libavutil/buffer.h>
}

namespace mw::streamer::ffmpeg {

class BufferRef final {
 public:
  ~BufferRef() noexcept;

  BufferRef(const BufferRef&) = delete;
  BufferRef& operator=(const BufferRef&) = delete;
  BufferRef(BufferRef&& other) noexcept;
  BufferRef& operator=(BufferRef&& other) noexcept;

  [[nodiscard]] static BufferRef Adopt(AVBufferRef* buffer);

  [[nodiscard]] AVBufferRef* get() noexcept;
  [[nodiscard]] const AVBufferRef* get() const noexcept;

  [[nodiscard]] BufferRef Ref() const;
  void Reset() noexcept;

 private:
  explicit BufferRef(AVBufferRef* buffer) noexcept;

  AVBufferRef* buffer_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_BUFFER_REF_H_

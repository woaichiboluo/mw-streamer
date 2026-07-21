#ifndef MW_STREAMER_FFMPEG_INPUT_FORMAT_CONTEXT_H_
#define MW_STREAMER_FFMPEG_INPUT_FORMAT_CONTEXT_H_

extern "C" {
#include <libavformat/avformat.h>
}

namespace mw::streamer::ffmpeg {

class InputFormatContext final {
 public:
  InputFormatContext();
  ~InputFormatContext() noexcept;

  InputFormatContext(const InputFormatContext&) = delete;
  InputFormatContext& operator=(const InputFormatContext&) = delete;
  InputFormatContext(InputFormatContext&& other) noexcept;
  InputFormatContext& operator=(InputFormatContext&& other) noexcept;

  [[nodiscard]] AVFormatContext* get() noexcept;
  [[nodiscard]] const AVFormatContext* get() const noexcept;
  [[nodiscard]] AVFormatContext** inout() noexcept;

  void Reset();

 private:
  void Release() noexcept;

  AVFormatContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_INPUT_FORMAT_CONTEXT_H_

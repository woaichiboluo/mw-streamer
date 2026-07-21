#ifndef MW_STREAMER_FFMPEG_FRAME_H_
#define MW_STREAMER_FFMPEG_FRAME_H_

extern "C" {
#include <libavutil/frame.h>
}

namespace mw::streamer::ffmpeg {

class Frame final {
 public:
  Frame();
  ~Frame() noexcept;

  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&& other) noexcept;
  Frame& operator=(Frame&& other) noexcept;

  [[nodiscard]] AVFrame* get() noexcept;
  [[nodiscard]] const AVFrame* get() const noexcept;

  void Unref() noexcept;
  [[nodiscard]] Frame Ref() const;
  void MakeWritable();

 private:
  AVFrame* frame_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_FRAME_H_

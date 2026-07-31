#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_H_

extern "C" {
#include <libavutil/frame.h>
}

namespace mw::streamer::ffmpeg {

class Frame final {
 public:
  Frame();
  // Adopts a frame allocated by av_frame_alloc() or av_frame_clone().
  explicit Frame(AVFrame* frame);
  ~Frame();

  Frame(const Frame& other);
  Frame& operator=(const Frame& other);
  Frame(Frame&& other) noexcept;
  Frame& operator=(Frame&& other) noexcept;

  static Frame Clone(const AVFrame& source);
  Frame Clone() const;
  Frame Ref() const;
  void CopyPropertiesFrom(const Frame& source);
  void ClearCrop() noexcept;

  const AVFrame* get() const noexcept;
  AVFrame* get() noexcept;
  const AVFrame* operator->() const noexcept;
  AVFrame* operator->() noexcept;
  void Unref() noexcept;

 private:
  AVFrame* frame_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_FRAME_H_

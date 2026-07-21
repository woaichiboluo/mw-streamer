#ifndef MW_STREAMER_FFMPEG_SWR_CONTEXT_H_
#define MW_STREAMER_FFMPEG_SWR_CONTEXT_H_

extern "C" {
#include <libswresample/swresample.h>
}

namespace mw::streamer::ffmpeg {

class SwrContext final {
 public:
  SwrContext();
  ~SwrContext() noexcept;

  SwrContext(const SwrContext&) = delete;
  SwrContext& operator=(const SwrContext&) = delete;
  SwrContext(SwrContext&& other) noexcept;
  SwrContext& operator=(SwrContext&& other) noexcept;

  [[nodiscard]] ::SwrContext* get() noexcept;
  [[nodiscard]] const ::SwrContext* get() const noexcept;
  [[nodiscard]] ::SwrContext** inout() noexcept;

 private:
  ::SwrContext* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_SWR_CONTEXT_H_

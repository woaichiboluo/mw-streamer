#ifndef MW_SRC_FRAME_CONVTER_CUDA_FRAME_POOL_H_
#define MW_SRC_FRAME_CONVTER_CUDA_FRAME_POOL_H_

#include <memory>
#include <mutex>
#include <new>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::frame_convter::internal {

struct BufferUnref {
  void operator()(AVBufferRef* value) const { av_buffer_unref(&value); }
};

// Single-device cache; callers bind the device's CUDA context before Get.
// Replacing the cache preserves the contexts retained by outstanding frames.
class FramePool final {
 public:
  streamer::ffmpeg::Frame Get(AVBufferRef* device, AVPixelFormat format,
                              int width, int height) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto* current =
        pool_ ? reinterpret_cast<const AVHWFramesContext*>(pool_->data)
              : nullptr;
    if (!current || current->sw_format != format || current->width != width ||
        current->height != height) {
      std::unique_ptr<AVBufferRef, BufferUnref> candidate(
          av_hwframe_ctx_alloc(device));
      if (!candidate) throw std::bad_alloc();
      auto* frames = reinterpret_cast<AVHWFramesContext*>(candidate->data);
      frames->format = AV_PIX_FMT_CUDA;
      frames->sw_format = format;
      frames->width = width;
      frames->height = height;
      streamer::ffmpeg::FfmpegException::throwIfError(
          av_hwframe_ctx_init(candidate.get()), "初始化 CUDA 帧池");
      pool_ = std::move(candidate);
    }
    streamer::ffmpeg::Frame output;
    streamer::ffmpeg::FfmpegException::throwIfError(
        av_hwframe_get_buffer(pool_.get(), output.get(), 0), "分配 CUDA 帧");
    return output;
  }

  void Discard(const streamer::ffmpeg::Frame& output) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (pool_ && pool_->data == output->hw_frames_ctx->data) {
      // Drop the lending reference before the failed output is released.
      // Other live frames retain the old context; future Get calls cannot
      // reuse its possibly still-in-flight failed buffer.
      pool_.reset();
    }
  }

 private:
  std::mutex mutex_;
  std::unique_ptr<AVBufferRef, BufferUnref> pool_;
};

}  // namespace mw::frame_convter::internal

#endif  // MW_SRC_FRAME_CONVTER_CUDA_FRAME_POOL_H_

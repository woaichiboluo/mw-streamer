#ifndef MW_STREAMER_INTERNAL_GPU_NV12_FRAME_POOL_H_
#define MW_STREAMER_INTERNAL_GPU_NV12_FRAME_POOL_H_

#include <optional>

#include "mw/streamer/ffmpeg/buffer_ref.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/cuda_device_context.h"

namespace mw::streamer::internal {

class GpuNv12FramePool final {
 public:
  void Create(const CudaDeviceContext& device_context, int width, int height,
              int initial_pool_size = 0);
  [[nodiscard]] ffmpeg::Frame Acquire();

  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] int device_id() const noexcept;
  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] const ffmpeg::BufferRef& buffer() const noexcept;

 private:
  std::optional<ffmpeg::BufferRef> frames_context_;
  int device_id_ = -1;
  int width_ = 0;
  int height_ = 0;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_GPU_NV12_FRAME_POOL_H_

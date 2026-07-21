#ifndef MW_STREAMER_INTERNAL_CUDA_DEVICE_CONTEXT_H_
#define MW_STREAMER_INTERNAL_CUDA_DEVICE_CONTEXT_H_

#include <optional>

#include "mw/streamer/ffmpeg/buffer_ref.h"

namespace mw::streamer::internal {

class CudaDeviceContext {
 public:
  CudaDeviceContext() = default;
  ~CudaDeviceContext() noexcept = default;

  CudaDeviceContext(const CudaDeviceContext&) = delete;
  CudaDeviceContext& operator=(const CudaDeviceContext&) = delete;
  CudaDeviceContext(CudaDeviceContext&&) noexcept = default;
  CudaDeviceContext& operator=(CudaDeviceContext&&) noexcept = default;

  void Create(int device_id);

  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] int device_id() const noexcept;
  [[nodiscard]] const ffmpeg::BufferRef& buffer() const noexcept;

 private:
  int device_id_ = -1;
  std::optional<ffmpeg::BufferRef> context_;
};

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_CUDA_DEVICE_CONTEXT_H_

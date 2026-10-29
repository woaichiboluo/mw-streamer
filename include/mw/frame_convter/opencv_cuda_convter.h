#ifndef MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_
#define MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_

#include <cuda_runtime_api.h>

#include <cstddef>
#include <memory>
#include <opencv2/core/cuda.hpp>

#include "mw/export.h"
#include "mw/frame_convter/rgb_order.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace mw::frame_convter {

// Stream-ordered frame conversion. ToMat/ToFrame run entirely on the GPU and
// support CUDA NV12/P010 frames with native color metadata.
// Unspecified colorspace uses BT709 for conversion; properties are preserved.
// HDR tone mapping and primaries/transfer-function changes are not supported.
// For ToMat/ToFrame, OpenCV's current CUDA context must match the frame's
// context; a mismatch throws std::invalid_argument without staging copies.
// Conversions preserve the selected device/context and enqueue work on stream
// (nullptr selects the default stream), without explicit synchronization.
// Callers establish producer dependencies and keep inputs, outputs, their
// device/context and stream alive until queued work completes. Input pixels
// must remain read-only during that time.
// Returned buffers are independently owned, but return does not imply that
// their pixels are ready. Callers order consumers and handle asynchronous
// errors at their own completion boundary, including after submission errors.
// Allocations and pageable host transfers may still block in the driver.
// Distinct input/output objects may be converted concurrently. Destroy the
// converter only after calls finish, GPU work completes, and all pool-backed
// GpuMats (including shallow copies and ROIs) have been released. Frames retain
// their own device/pool references and may outlive the converter.
class MW_FRAME_CONVTER_API OpencvCudaConvter final {
 public:
  // Retains an initialized CUDA device and allocates pool_bytes on its context,
  // restoring the calling thread's context. Zero disables the GpuMat pool.
  // The first fitting ToMat output determines the fixed block shape. A full
  // pool, a different shape, or an oversized output uses OpenCV's standard
  // allocator. The budget limits the pool, not total live output memory.
  explicit OpencvCudaConvter(const streamer::ffmpeg::HwDeviceContext& device,
                             std::size_t pool_bytes = 256ULL * 1024 * 1024);
  ~OpencvCudaConvter();

  OpencvCudaConvter(const OpencvCudaConvter&) = delete;
  OpencvCudaConvter& operator=(const OpencvCudaConvter&) = delete;
  OpencvCudaConvter(OpencvCudaConvter&&) = delete;
  OpencvCudaConvter& operator=(OpencvCudaConvter&&) = delete;

  // Allocates uninitialized CUDA pixels from the reusable ToFrame pool.
  // reference supplies an initialized NV12/P010 hw_frames_ctx in the bound
  // context and properties; it need not contain pixels. width/height must be
  // valid even dimensions. Properties are copied and crop is cleared.
  // Temporarily binds the bound context and restores the caller's context.
  // Does not copy pixels, submit a conversion, or explicitly synchronize.
  // The returned frame retains its own pool/device references.
  streamer::ffmpeg::Frame allocate(const streamer::ffmpeg::Frame& reference,
                                   int width, int height);

  // Uploads CPU pixels without changing layout, dimensions or properties.
  // The bound device must support the source layout.
  // Unsupported layouts/invalid inputs throw std::invalid_argument. Transfer
  // errors propagate as exceptions. Temporarily binds device's context and
  // restores the calling thread's context on return or exception. stream must
  // belong to device's context; nullptr selects that context's default stream.
  // Completion checks must use that context, even after this call restores a
  // different calling context. Output retains its pool/device references.
  // Source pixels must remain alive and read-only until stream completes;
  // pinned host memory is needed for reliably asynchronous host transfers.
  streamer::ffmpeg::Frame CpuFrameToCuda(const streamer::ffmpeg::Frame& source,
                                         cudaStream_t stream = nullptr);

  // Produces CV_8UC3 on OpenCV's current device, quantizing RGB to eight bits
  // per channel. NV12/P010 source frames must have initialized hw_frames_ctx.
  // The frame must belong to this converter's CUDA context.
  cv::cuda::GpuMat ToMat(const streamer::ffmpeg::Frame& frame,
                         RgbOrder order = RgbOrder::kBgr,
                         cudaStream_t stream = nullptr);

  // Accepts CV_8UC3 on OpenCV's current CUDA context (including pitched ROIs).
  // reference supplies hw_frames_ctx/device, underlying NV12/P010 format and
  // properties; it need not contain pixels. Dimensions come from image and
  // must be even. Crop is cleared. The output retains its pool/device refs.
  // Invalid input, unsupported color metadata and context mismatches throw
  // std::invalid_argument.
  // reference must belong to this converter's CUDA context.
  streamer::ffmpeg::Frame ToFrame(const cv::cuda::GpuMat& image,
                                  const streamer::ffmpeg::Frame& reference,
                                  RgbOrder order = RgbOrder::kBgr,
                                  cudaStream_t stream = nullptr);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::frame_convter

#endif  // MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_

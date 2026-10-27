#ifndef MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_
#define MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_

#include <opencv2/core/cuda.hpp>

#include "mw/export.h"
#include "mw/frame_convter/rgb_order.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"

namespace mw::frame_convter {

// Synchronous frame conversion. ToMat/ToFrame run entirely on the GPU and
// support CUDA NV12/P010 frames with native color metadata.
// Unspecified colorspace uses BT709 for conversion; properties are preserved.
// HDR tone mapping and primaries/transfer-function changes are not supported.
// For ToMat/ToFrame, OpenCV's current CUDA context must match the frame's
// context; a mismatch throws std::invalid_argument without staging copies.
// Calls preserve the selected device/context. Inputs must remain alive and
// read-only for the call. Independently owned results remain valid after
// inputs are destroyed.
class MW_FRAME_CONVTER_API OpencvCudaConvter final {
 public:
  OpencvCudaConvter() = delete;

  // Uploads CPU pixels without changing layout, dimensions or properties.
  // device must be an initialized CUDA device supporting the source layout.
  // Unsupported layouts/invalid inputs throw std::invalid_argument. Transfer
  // errors propagate as exceptions. Temporarily binds device's context and
  // restores the calling thread's context on return or exception. The upload
  // completes before return; output retains its pool/device references and
  // remains valid after source/device wrappers are destroyed. Concurrent calls
  // require an externally initialized device and read-only source pixels.
  static streamer::ffmpeg::Frame CpuFrameToCuda(
      const streamer::ffmpeg::Frame& source,
      const streamer::ffmpeg::HwDeviceContext& device);

  // Produces CV_8UC3 on OpenCV's current device, quantizing RGB to eight bits
  // per channel. NV12/P010 source frames must have initialized hw_frames_ctx.
  static cv::cuda::GpuMat ToMat(const streamer::ffmpeg::Frame& frame,
                                RgbOrder order = RgbOrder::kBgr);

  // Accepts CV_8UC3 on OpenCV's current CUDA context (including pitched ROIs).
  // reference supplies hw_frames_ctx/device, underlying NV12/P010 format and
  // properties; it need not contain pixels. Dimensions come from image and
  // must be even. Crop is cleared. The output retains its pool/device refs.
  // Invalid input, unsupported color metadata and context mismatches throw
  // std::invalid_argument.
  static streamer::ffmpeg::Frame ToFrame(
      const cv::cuda::GpuMat& image, const streamer::ffmpeg::Frame& reference,
      RgbOrder order = RgbOrder::kBgr);
};

}  // namespace mw::frame_convter

#endif  // MW_FRAME_CONVTER_OPENCV_CUDA_CONVTER_H_

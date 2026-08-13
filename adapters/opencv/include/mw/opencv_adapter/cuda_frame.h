#ifndef MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_FRAME_H_
#define MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_FRAME_H_

#include <memory>

#include "mw/opencv_adapter/host_frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::opencv_adapter {

class CudaMatAdapter;

// Owns a pitched CUDA copy of a linear video frame. Allocation follows
// cv::cuda::GpuMat semantics: it uses the CUDA context current on the calling
// thread, or the current device's primary context when none is current.
// The allocation context must outlive this object.
class CudaFrame final {
 public:
  static CudaFrame CopyFrom(const MwStreamerVideoFrameView& source);

  ~CudaFrame();

  CudaFrame(const CudaFrame&) = delete;
  CudaFrame& operator=(const CudaFrame&) = delete;
  CudaFrame(CudaFrame&& other) noexcept;
  CudaFrame& operator=(CudaFrame&& other) noexcept;

  HostFrame ToHost() const;

  // Synchronously copies this frame into a matching writable output buffer.
  // Host and CUDA linear destinations are supported.
  void CopyTo(const MwStreamerVideoBufferView& destination) const;

  // The returned view remains valid until this CudaFrame is moved from or
  // destroyed. A moved-from CudaFrame may only be assigned to or destroyed.
  const MwStreamerVideoFrameView& view() const noexcept;

 private:
  friend class CudaMatAdapter;

  class Impl;

  static CudaFrame AllocateLike(const MwStreamerVideoFrameView& prototype);

  explicit CudaFrame(std::unique_ptr<Impl> impl) noexcept;

  MwStreamerVideoFrameView& mutable_view() noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer::opencv_adapter

#endif  // MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_FRAME_H_

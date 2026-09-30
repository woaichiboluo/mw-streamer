#ifndef MW_OPENCV_ADAPTER_CUDA_FRAME_H_
#define MW_OPENCV_ADAPTER_CUDA_FRAME_H_

#include <cuda.h>

#include <memory>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

// Owns reusable linear CUDA storage in an explicitly selected context.
// Allocation is a setup operation; frame processing uses Copy and does not
// allocate, stage, or synchronize.
class MW_OPENCV_ADAPTER_API CudaFrame final {
 public:
  // Allocates all planes in one CUDA allocation owned by context. The caller
  // must keep context alive until the returned frame is destroyed.
  static CudaFrame Allocate(const MwStreamerVideoFrameView& prototype,
                            CUcontext context);

  // Submits a copy into stream and returns without synchronizing. A null stream
  // selects the CUDA default stream in the CUDA endpoint's context. Every CUDA
  // pointer and an explicit stream must belong to that context, and all strides
  // must be positive.
  // Page-locked Host memory enables true asynchronous DMA; pageable Host memory
  // is also accepted, but the CUDA driver may stage it and block the submitting
  // thread. The adapter never allocates or registers memory during this call.
  // Adjacent planes with identical pitches and row widths are submitted as one
  // 2D copy. Other layouts use one 2D copy per plane, never one call per row.
  static void Copy(const MwStreamerVideoFrameView& source,
                   const MwStreamerVideoBufferView& destination,
                   CUstream stream = nullptr);

  ~CudaFrame();

  CudaFrame(const CudaFrame&) = delete;
  CudaFrame& operator=(const CudaFrame&) = delete;
  CudaFrame(CudaFrame&& other) noexcept;
  CudaFrame& operator=(CudaFrame&& other) noexcept;

  void CopyFrom(const MwStreamerVideoFrameView& source,
                CUstream stream = nullptr);
  void CopyTo(const MwStreamerVideoBufferView& destination,
              CUstream stream = nullptr) const;

  // The returned view remains valid until this CudaFrame is moved from or
  // destroyed. A moved-from CudaFrame may only be assigned to or destroyed.
  const MwStreamerVideoFrameView& view() const noexcept;
  CUcontext context() const noexcept;

 private:
  class Impl;

  explicit CudaFrame(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_CUDA_FRAME_H_

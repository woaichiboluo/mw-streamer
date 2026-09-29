#ifndef MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_
#define MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_

#include <cuda.h>

#include <opencv2/core/cuda.hpp>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

class MW_OPENCV_ADAPTER_API CudaMatAdapter final {
 public:
  // Submits a CUDA YUV-to-BGR conversion without copying or owning either
  // frame. source and destination must be linear CUDA storage allocated in
  // context. destination must already have the required size and type.
  //
  // The function temporarily makes context current when necessary and submits
  // all work to stream. A null stream selects the CUDA default stream in
  // context. It does not synchronize. The caller must keep every referenced
  // allocation alive until stream completes. Cross-context input, output, or
  // explicit stream use is an error and is never copied implicitly.
  //
  // This allocation-free path supports NV12, P010, P016, YUV420P, YUV422P,
  // YUV420P10LE, and YUV422P10LE. Planar YUV444 requires a fused,
  // allocation-free implementation before it can use this API.
  static void ToBgr(const MwStreamerVideoFrameView& source,
                    cv::cuda::GpuMat* destination, CUcontext context,
                    CUstream stream = nullptr);

  // Submits a CUDA BGR-to-YUV conversion directly into the borrowed output.
  // destination_color describes the color encoding written to destination.
  // Ownership, context, stream, and lifetime restrictions are the same as
  // ToBgr. The allocation-free output path supports NV12, P010, P016,
  // YUV420P, and YUV420P10LE. Planar YUV422/444 require fused allocation-free
  // implementations before they can use this API.
  // destination is not safe to consume until stream completes.
  static void FromBgr(const cv::cuda::GpuMat& source,
                      const MwStreamerVideoColorInfo& destination_color,
                      const MwStreamerVideoBufferView& destination,
                      CUcontext context, CUstream stream = nullptr);

  CudaMatAdapter() = delete;
};

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_

#ifndef MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_
#define MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_

#include <opencv2/core/cuda.hpp>

#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::opencv_adapter {

class CudaMatAdapter final {
 public:
  // Converts a supported Host or CUDA YUV view to an owning CUDA BGR image.
  // The conversion is complete when this function returns.
  static cv::cuda::GpuMat ToBgr(const MwStreamerVideoFrameView& source);

  // Converts a CUDA BGR image to a CUDA frame whose raw format and metadata
  // are copied from prototype. The conversion is complete on return.
  static CudaFrame FromBgr(const cv::cuda::GpuMat& source,
                           const MwStreamerVideoFrameView& prototype);

  CudaMatAdapter() = delete;
};

}  // namespace mw::streamer::opencv_adapter

#endif  // MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_CUDA_MAT_ADAPTER_H_

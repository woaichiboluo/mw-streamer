#ifndef MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_
#define MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

#include <opencv2/core/mat.hpp>

#include "mw/opencv_adapter/host_frame.h"
#include "mw/processor/processor.h"

namespace mw::streamer::opencv_adapter {

class HostMatAdapter final {
 public:
  // Converts a supported Host or CUDA YUV view to an owning Host BGR image.
  static cv::Mat ToBgr(const MwStreamerVideoFrameView& source);

  // Converts an owning OpenCV BGR image to a Host frame whose raw format and
  // metadata are copied from prototype.
  static HostFrame FromBgr(const cv::Mat& source,
                           const MwStreamerVideoFrameView& prototype);

  HostMatAdapter() = delete;
};

}  // namespace mw::streamer::opencv_adapter

#endif  // MW_STREAMER_ADAPTERS_OPENCV_INCLUDE_MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

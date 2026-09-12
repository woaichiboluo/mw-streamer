#ifndef MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_
#define MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

#include <opencv2/core/mat.hpp>

#include "mw/export.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

class MW_OPENCV_ADAPTER_API HostMatAdapter final {
 public:
  // Converts a supported Host or CUDA YUV view to an owning Host BGR image.
  static cv::Mat ToBgr(const MwStreamerVideoFrameView& source);

  // Converts an owning OpenCV BGR image to a Host frame whose raw format and
  // metadata are copied from prototype.
  static HostFrame FromBgr(const cv::Mat& source,
                           const MwStreamerVideoFrameView& prototype);

  HostMatAdapter() = delete;
};

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

#ifndef MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_
#define MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

#include <opencv2/core/mat.hpp>

#include "mw/export.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

class MW_OPENCV_ADAPTER_API HostMatAdapter final {
 public:
  // Converts a supported Host YUV view to an owning Host BGR image.
  static cv::Mat ToBgr(const MwStreamerVideoFrameView& source);

  // Converts an owning OpenCV BGR image to a Host frame whose raw format and
  // metadata are copied from prototype.
  static HostFrame FromBgr(const cv::Mat& source,
                           const MwStreamerVideoFrameView& prototype);

  // Converts BGR directly into a borrowed Host output. No intermediate frame
  // or pixel copy is created; destination may have positive stride padding.
  static void ConvertFromBgr(const cv::Mat& source,
                             const MwStreamerVideoColorInfo& destination_color,
                             const MwStreamerVideoBufferView& destination);

  HostMatAdapter() = delete;
};

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_HOST_MAT_ADAPTER_H_

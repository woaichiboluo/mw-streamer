#ifndef MW_FRAME_CONVTER_OPENCV_CPU_CONVTER_H_
#define MW_FRAME_CONVTER_OPENCV_CPU_CONVTER_H_

#include <opencv2/core/mat.hpp>

#include "mw/export.h"
#include "mw/frame_convter/rgb_order.h"
#include "mw/streamer/ffmpeg/frame.h"

namespace mw::frame_convter {

// CPU pixel conversion only: no resizing, device transfers, tone mapping or
// changes to primaries/transfer functions. YUV conversions require native
// AVFrame color_range. Unspecified colorspace uses BT709 for conversion;
// properties are preserved. Unsupported matrices and missing ranges throw.
// Calls are synchronous and independent, and may run concurrently. Inputs must
// remain alive and read-only for the call. Returned buffers are independently
// owned; an OpenCV ROI and padded/negative FFmpeg linesizes are supported.
class MW_FRAME_CONVTER_API OpencvCpuConvter final {
 public:
  OpencvCpuConvter() = delete;

  // Produces CV_8UC3 in OpenCV's BGR order by default. Converting a
  // high-bit-depth frame to this image quantizes to 8 bits per RGB channel.
  static cv::Mat ToMat(const streamer::ffmpeg::Frame& frame,
                       RgbOrder order = RgbOrder::kBgr);

  // Accepts CV_8UC3. Output dimensions come from image; format and properties
  // come from reference. Reference is a CPU frame/template and need not contain
  // pixels. Crop offsets are cleared because the returned pixels are new.
  static streamer::ffmpeg::Frame ToFrame(
      const cv::Mat& image, const streamer::ffmpeg::Frame& reference,
      RgbOrder order = RgbOrder::kBgr);

  // Converts directly between supported CPU formats without an RGB8
  // intermediate. Output dimensions come from source, format/properties from
  // reference. Unsupported conversions and invalid arguments throw; there is
  // no device fallback. Does not modify either input.
  static streamer::ffmpeg::Frame Convert(
      const streamer::ffmpeg::Frame& source,
      const streamer::ffmpeg::Frame& reference);
};

}  // namespace mw::frame_convter

#endif  // MW_FRAME_CONVTER_OPENCV_CPU_CONVTER_H_

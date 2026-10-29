#include <cmath>
#include <stdexcept>

#include "mw/frame_convter.h"

int main() {
  namespace ffmpeg = mw::streamer::ffmpeg;
  using mw::frame_convter::OpencvCpuConvter;
  auto volatile allocate = &mw::frame_convter::OpencvCudaConvter::allocate;
  if (allocate == nullptr) return 1;
  ffmpeg::Frame reference;
  reference->format = AV_PIX_FMT_YUV420P;
  reference->colorspace = AVCOL_SPC_BT709;
  reference->color_range = AVCOL_RANGE_MPEG;
  reference->pts = 42;
  const cv::Mat image(48, 64, CV_8UC3, cv::Scalar(40, 120, 200));
  const auto frame = OpencvCpuConvter::ToFrame(image, reference);
  const auto restored = OpencvCpuConvter::ToMat(frame);
  // Resolve the instance API and its static dependencies without requiring a
  // GPU. A CPU device must be rejected before initializing CUDA.
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  try {
    mw::frame_convter::OpencvCudaConvter converter(cpu);
    return 1;
  } catch (const std::invalid_argument&) {
  }
  if (frame->pts != 42 || frame->width != 64 || frame->height != 48) return 1;
  const auto pixel = restored.at<cv::Vec3b>(0, 0);
  return std::abs(static_cast<int>(pixel[0]) - 40) <= 4 &&
                 std::abs(static_cast<int>(pixel[1]) - 120) <= 4 &&
                 std::abs(static_cast<int>(pixel[2]) - 200) <= 4
             ? 0
             : 1;
}

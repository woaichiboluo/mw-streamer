#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_VIDEO_FRAME_ALLOCATOR_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_VIDEO_FRAME_ALLOCATOR_H_

#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

#include "mw/ffmpeg/frame.h"

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::processor::internal {

class VideoFrameAllocator final {
 public:
  VideoFrameAllocator(std::uint32_t output_width, std::uint32_t output_height);
  ~VideoFrameAllocator();

  VideoFrameAllocator(const VideoFrameAllocator&) = delete;
  VideoFrameAllocator& operator=(const VideoFrameAllocator&) = delete;
  VideoFrameAllocator(VideoFrameAllocator&&) = delete;
  VideoFrameAllocator& operator=(VideoFrameAllocator&&) = delete;

  // The first allocation captures the input storage and lazily creates a CUDA
  // output frames context when required. Later inputs must preserve the same
  // format, dimensions, and hardware device.
  ffmpeg::Frame Allocate(const ffmpeg::Frame& input);

  // Validates every input before returning a cached black frame. The cache is
  // rebuilt when the input color range changes.
  ffmpeg::Frame GetBlackFrame(const ffmpeg::Frame& input,
                              const ffmpeg::HardwareContext* hardware_context);

 private:
  ffmpeg::Frame AllocateBlackFrame(const ffmpeg::Frame& input);
  void PrepareOrValidate(const AVFrame& input);
  void Prepare(const AVFrame& input);
  void ValidatePreparedInput(const AVFrame& input) const;

  std::uint32_t output_width_;
  std::uint32_t output_height_;
  AVPixelFormat input_format_ = AV_PIX_FMT_NONE;
  AVPixelFormat storage_format_ = AV_PIX_FMT_NONE;
  int input_width_ = 0;
  int input_height_ = 0;
  AVBufferRef* input_device_context_ = nullptr;
  AVBufferRef* output_frames_context_ = nullptr;
  std::optional<ffmpeg::Frame> black_frame_;
  bool prepared_ = false;
};

}  // namespace mw::streamer::processor::internal

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_VIDEO_FRAME_ALLOCATOR_H_

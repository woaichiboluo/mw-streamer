#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace mw::streamer::ffmpeg {

class HardwareContext final {
 public:
  static HardwareContext CreateCuda(int device_index);

  // Returns the frame's borrowed hardware frames context when its FFmpeg
  // ownership links and formats are internally consistent.
  static const AVHWFramesContext* GetFramesContext(
      const AVFrame& frame) noexcept;

  ~HardwareContext();

  HardwareContext(const HardwareContext& other);
  HardwareContext& operator=(const HardwareContext& other);
  HardwareContext(HardwareContext&& other) noexcept;
  HardwareContext& operator=(HardwareContext&& other) noexcept;

  AVHWDeviceType type() const noexcept;
  int device_index() const noexcept;
  const AVBufferRef* get() const noexcept;

  // Returns whether the frame belongs to this exact FFmpeg hardware device
  // context. Contexts created independently for the same physical device are
  // not interchangeable.
  bool IsCompatible(const AVFrame& frame) const noexcept;

 private:
  HardwareContext(AVBufferRef* context, int device_index);

  AVBufferRef* context_ = nullptr;
  int device_index_ = -1;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_

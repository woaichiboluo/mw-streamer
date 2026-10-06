#ifndef MW_STREAMER_FFMPEG_HW_DEVICE_CONTEXT_H_
#define MW_STREAMER_FFMPEG_HW_DEVICE_CONTEXT_H_

#include <string_view>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "mw/export.h"

namespace mw::streamer::ffmpeg {

enum class HwDeviceType { kCpu, kCuda };

// Owns one FFmpeg reference to a hardware device. Copies share the device and
// its native context (for example, CUDA's CUcontext); they do not recreate it.
// CPU contexts have no hardware reference; get() returns nullptr.
class MW_STREAMER_API HwDeviceContext final {
 public:
  // An empty device selects FFmpeg's default device. Options remain owned by
  // the caller. Creation failures throw; no other device is tried.
  explicit HwDeviceContext(HwDeviceType type, std::string_view device = {},
                           AVDictionary* options = nullptr, int flags = 0);
  // Adopts an existing reference to an initialized AVHWDeviceContext.
  explicit HwDeviceContext(AVBufferRef* context);
  ~HwDeviceContext();

  HwDeviceContext(const HwDeviceContext& other);
  HwDeviceContext& operator=(const HwDeviceContext& other);
  HwDeviceContext(HwDeviceContext&& other) noexcept;
  HwDeviceContext& operator=(HwDeviceContext&& other) noexcept;

  HwDeviceContext Ref() const;
  // Borrowed reference. Use av_buffer_ref() before assigning it to a codec's
  // hw_device_ctx, since libavcodec takes ownership of that reference. For CPU,
  // leave hw_device_ctx null instead.
  const AVBufferRef* get() const noexcept;
  AVBufferRef* get() noexcept;

 private:
  AVBufferRef* context_ = nullptr;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_HW_DEVICE_CONTEXT_H_

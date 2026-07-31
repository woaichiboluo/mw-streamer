#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_

#include <cstdint>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace mw::streamer::ffmpeg {

class HardwareContext final {
 public:
  class CurrentScope final {
   public:
    ~CurrentScope();

    CurrentScope(const CurrentScope&) = delete;
    CurrentScope& operator=(const CurrentScope&) = delete;
    CurrentScope(CurrentScope&&) = delete;
    CurrentScope& operator=(CurrentScope&&) = delete;

   private:
    friend class HardwareContext;

    explicit CurrentScope(const AVBufferRef* context);

    AVBufferRef* context_ = nullptr;
    bool pushed_ = false;
  };

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

  // The returned backend execution handle is borrowed and remains valid while
  // any copy of this HardwareContext or an FFmpeg child context is alive.
  std::uintptr_t native_handle() const noexcept;

  // Returns whether the frame belongs to this exact FFmpeg hardware device
  // context. Contexts created independently for the same physical device are
  // not interchangeable because they may use different execution streams.
  bool IsCompatible(const AVFrame& frame) const noexcept;

  // Makes this device's backend context current on the calling thread. The
  // scope must be destroyed on the same thread on which it was created.
  CurrentScope MakeCurrent() const;

  // Waits for work submitted to this context's execution handle. The method
  // makes and restores the backend context internally.
  void Synchronize() const;

 private:
  HardwareContext(AVBufferRef* context, int device_index);

  AVBufferRef* context_ = nullptr;
  int device_index_ = -1;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_HARDWARE_CONTEXT_H_

#include "mw/ffmpeg/hardware_context.h"

#include <fmt/format.h>

#include <new>
#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/pixel_format.h"

namespace mw::streamer::ffmpeg {

HardwareContext HardwareContext::CreateCuda(int device_index) {
  if (device_index < 0) {
    throw std::invalid_argument("CUDA设备索引不能为负数");
  }

  AVBufferRef* device_ref = nullptr;
  const auto device_name = fmt::format("{}", device_index);
  ThrowIfError(av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_CUDA,
                                      device_name.c_str(), nullptr, 0),
               "创建FFmpeg CUDA硬件上下文");

  return HardwareContext(device_ref, device_index);
}

const AVHWFramesContext* HardwareContext::GetFramesContext(
    const AVFrame& frame) noexcept {
  const auto format = static_cast<AVPixelFormat>(frame.format);
  if (!IsHardwarePixelFormat(format) || !frame.hw_frames_ctx ||
      !frame.hw_frames_ctx->data) {
    return nullptr;
  }

  const auto* frames_context =
      reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
  if (frames_context->format != format ||
      frames_context->sw_format == AV_PIX_FMT_NONE ||
      !frames_context->device_ref || !frames_context->device_ref->data ||
      !frames_context->device_ctx ||
      frames_context->device_ctx != reinterpret_cast<const AVHWDeviceContext*>(
                                        frames_context->device_ref->data) ||
      frames_context->device_ctx->type == AV_HWDEVICE_TYPE_NONE) {
    return nullptr;
  }
  return frames_context;
}

HardwareContext::HardwareContext(AVBufferRef* context, int device_index)
    : context_(context), device_index_(device_index) {}

HardwareContext::~HardwareContext() { av_buffer_unref(&context_); }

HardwareContext::HardwareContext(const HardwareContext& other) {
  if (!other.context_) {
    throw std::logic_error("不能引用已移动的HardwareContext");
  }
  context_ = av_buffer_ref(other.context_);
  if (!context_) {
    throw std::bad_alloc();
  }
  device_index_ = other.device_index_;
}

HardwareContext& HardwareContext::operator=(const HardwareContext& other) {
  if (this != &other) {
    HardwareContext copy(other);
    std::swap(context_, copy.context_);
    std::swap(device_index_, copy.device_index_);
  }
  return *this;
}

HardwareContext::HardwareContext(HardwareContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      device_index_(std::exchange(other.device_index_, -1)) {}

HardwareContext& HardwareContext::operator=(HardwareContext&& other) noexcept {
  if (this != &other) {
    av_buffer_unref(&context_);
    context_ = std::exchange(other.context_, nullptr);
    device_index_ = std::exchange(other.device_index_, -1);
  }
  return *this;
}

AVHWDeviceType HardwareContext::type() const noexcept {
  if (!context_) {
    return AV_HWDEVICE_TYPE_NONE;
  }
  const auto* device_context =
      reinterpret_cast<const AVHWDeviceContext*>(context_->data);
  return device_context ? device_context->type : AV_HWDEVICE_TYPE_NONE;
}

int HardwareContext::device_index() const noexcept { return device_index_; }

const AVBufferRef* HardwareContext::get() const noexcept { return context_; }

bool HardwareContext::IsCompatible(const AVFrame& frame) const noexcept {
  const auto* frames_context = GetFramesContext(frame);
  if (!context_ || !frames_context) {
    return false;
  }

  return frames_context->device_ref && frames_context->device_ctx &&
         frames_context->device_ref->data == context_->data &&
         frames_context->device_ctx ==
             reinterpret_cast<const AVHWDeviceContext*>(context_->data) &&
         frames_context->device_ctx->type == type();
}

}  // namespace mw::streamer::ffmpeg

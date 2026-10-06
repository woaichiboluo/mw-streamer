#include "mw/streamer/ffmpeg/hw_device_context.h"

#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

AVHWDeviceType ToAvHwDeviceType(HwDeviceType type) {
  switch (type) {
    case HwDeviceType::kCpu:
      return AV_HWDEVICE_TYPE_NONE;
    case HwDeviceType::kCuda:
      return AV_HWDEVICE_TYPE_CUDA;
  }
  throw std::invalid_argument("不支持的硬件设备类型");
}

}  // namespace

HwDeviceContext::HwDeviceContext(HwDeviceType type, std::string_view device,
                                 AVDictionary* options, int flags) {
  const auto av_type = ToAvHwDeviceType(type);
  if (av_type == AV_HWDEVICE_TYPE_NONE) {
    return;
  }
  const std::string device_name(device);
  FfmpegException::throwIfError(
      av_hwdevice_ctx_create(
          &context_, av_type,
          device_name.empty() ? nullptr : device_name.c_str(), options, flags),
      "创建硬件设备上下文");
}

HwDeviceContext::HwDeviceContext(AVBufferRef* context) : context_(context) {
  if (!context_) {
    throw std::invalid_argument("不能接管空硬件设备上下文");
  }
}

HwDeviceContext::~HwDeviceContext() { av_buffer_unref(&context_); }

HwDeviceContext::HwDeviceContext(const HwDeviceContext& other) {
  if (!other.context_) {
    return;
  }
  context_ = av_buffer_ref(other.context_);
  if (!context_) {
    throw std::bad_alloc();
  }
}

HwDeviceContext& HwDeviceContext::operator=(const HwDeviceContext& other) {
  if (this != &other) {
    HwDeviceContext copy(other);
    std::swap(context_, copy.context_);
  }
  return *this;
}

HwDeviceContext::HwDeviceContext(HwDeviceContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

HwDeviceContext& HwDeviceContext::operator=(HwDeviceContext&& other) noexcept {
  if (this != &other) {
    av_buffer_unref(&context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

HwDeviceContext HwDeviceContext::Ref() const { return HwDeviceContext(*this); }

const AVBufferRef* HwDeviceContext::get() const noexcept { return context_; }

AVBufferRef* HwDeviceContext::get() noexcept { return context_; }

}  // namespace mw::streamer::ffmpeg

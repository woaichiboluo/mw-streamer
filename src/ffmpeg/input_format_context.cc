#include "mw/ffmpeg/input_format_context.h"

#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/packet.h"

namespace mw::streamer::ffmpeg {

InputFormatContext::InputFormatContext(const std::string& input,
                                       AVIOInterruptCB interrupt_callback) {
  if (input.empty()) {
    throw std::invalid_argument("FFmpeg输入地址不能为空");
  }

  context_ = avformat_alloc_context();
  if (!context_) {
    throw std::bad_alloc();
  }
  context_->interrupt_callback = interrupt_callback;

  const int result =
      avformat_open_input(&context_, input.c_str(), nullptr, nullptr);
  if (result < 0) {
    if (context_) {
      avformat_close_input(&context_);
    }
    ThrowIfError(result, "打开FFmpeg输入");
  }
}

InputFormatContext::~InputFormatContext() { avformat_close_input(&context_); }

InputFormatContext::InputFormatContext(InputFormatContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

InputFormatContext& InputFormatContext::operator=(
    InputFormatContext&& other) noexcept {
  if (this != &other) {
    avformat_close_input(&context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

const AVFormatContext* InputFormatContext::get() const noexcept {
  return context_;
}

AVFormatContext* InputFormatContext::get() noexcept { return context_; }

const AVFormatContext* InputFormatContext::operator->() const noexcept {
  return context_;
}

AVFormatContext* InputFormatContext::operator->() noexcept { return context_; }

void InputFormatContext::FindStreamInfo() {
  if (!context_) {
    throw std::logic_error("不能读取已移动的InputFormatContext流信息");
  }
  ThrowIfError(avformat_find_stream_info(context_, nullptr),
               "读取FFmpeg输入流信息");
}

bool InputFormatContext::ReadPacket(Packet& packet) {
  if (!context_) {
    throw std::logic_error("不能读取已移动的InputFormatContext");
  }
  const int result = av_read_frame(context_, packet.get());
  if (result == AVERROR_EOF) {
    return false;
  }
  ThrowIfError(result, "读取FFmpeg输入媒体包");
  return true;
}

}  // namespace mw::streamer::ffmpeg

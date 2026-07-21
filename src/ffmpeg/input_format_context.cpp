#include "mw/streamer/ffmpeg/input_format_context.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {
namespace {

AVFormatContext* AllocateContext() {
  AVFormatContext* context = avformat_alloc_context();
  if (context == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate FFmpeg input format context");
  }
  return context;
}

}  // namespace

InputFormatContext::InputFormatContext() : context_(AllocateContext()) {}

InputFormatContext::~InputFormatContext() noexcept { Release(); }

InputFormatContext::InputFormatContext(InputFormatContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

InputFormatContext& InputFormatContext::operator=(
    InputFormatContext&& other) noexcept {
  if (this != &other) {
    Release();
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

AVFormatContext* InputFormatContext::get() noexcept { return context_; }

const AVFormatContext* InputFormatContext::get() const noexcept {
  return context_;
}

AVFormatContext** InputFormatContext::inout() noexcept { return &context_; }

void InputFormatContext::Reset() {
  AVFormatContext* replacement = AllocateContext();
  Release();
  context_ = replacement;
}

void InputFormatContext::Release() noexcept {
  if (context_ == nullptr) {
    return;
  }
  if (context_->iformat != nullptr) {
    avformat_close_input(&context_);
    return;
  }
  avformat_free_context(context_);
  context_ = nullptr;
}

}  // namespace mw::streamer::ffmpeg

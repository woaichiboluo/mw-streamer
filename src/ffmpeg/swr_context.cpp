#include "mw/streamer/ffmpeg/swr_context.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

SwrContext::SwrContext() : context_(swr_alloc()) {
  if (context_ == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate FFmpeg resampling context");
  }
}

SwrContext::~SwrContext() noexcept { swr_free(&context_); }

SwrContext::SwrContext(SwrContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

SwrContext& SwrContext::operator=(SwrContext&& other) noexcept {
  if (this != &other) {
    swr_free(&context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

::SwrContext* SwrContext::get() noexcept { return context_; }

const ::SwrContext* SwrContext::get() const noexcept { return context_; }

::SwrContext** SwrContext::inout() noexcept { return &context_; }

}  // namespace mw::streamer::ffmpeg

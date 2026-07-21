#include "mw/streamer/ffmpeg/buffer_ref.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

BufferRef::BufferRef(AVBufferRef* buffer) noexcept : buffer_(buffer) {}

BufferRef::~BufferRef() noexcept { av_buffer_unref(&buffer_); }

BufferRef::BufferRef(BufferRef&& other) noexcept
    : buffer_(std::exchange(other.buffer_, nullptr)) {}

BufferRef& BufferRef::operator=(BufferRef&& other) noexcept {
  if (this != &other) {
    av_buffer_unref(&buffer_);
    buffer_ = std::exchange(other.buffer_, nullptr);
  }
  return *this;
}

BufferRef BufferRef::Adopt(AVBufferRef* buffer) {
  if (buffer == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot adopt an empty FFmpeg buffer reference");
  }
  return BufferRef(buffer);
}

AVBufferRef* BufferRef::get() noexcept { return buffer_; }

const AVBufferRef* BufferRef::get() const noexcept { return buffer_; }

BufferRef BufferRef::Ref() const {
  if (buffer_ == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot reference an empty FFmpeg buffer");
  }

  AVBufferRef* reference = av_buffer_ref(buffer_);
  if (reference == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to reference an FFmpeg buffer");
  }
  return BufferRef(reference);
}

void BufferRef::Reset() noexcept { av_buffer_unref(&buffer_); }

}  // namespace mw::streamer::ffmpeg

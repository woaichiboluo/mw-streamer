#include "mw/streamer/ffmpeg/frame.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

Frame::Frame() : frame_(av_frame_alloc()) {
  if (frame_ == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate an FFmpeg frame");
  }
}

Frame::~Frame() noexcept { av_frame_free(&frame_); }

Frame::Frame(Frame&& other) noexcept
    : frame_(std::exchange(other.frame_, nullptr)) {}

Frame& Frame::operator=(Frame&& other) noexcept {
  if (this != &other) {
    av_frame_free(&frame_);
    frame_ = std::exchange(other.frame_, nullptr);
  }
  return *this;
}

AVFrame* Frame::get() noexcept { return frame_; }

const AVFrame* Frame::get() const noexcept { return frame_; }

void Frame::Unref() noexcept {
  if (frame_ != nullptr) {
    av_frame_unref(frame_);
  }
}

Frame Frame::Ref() const {
  if (frame_ == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot reference an empty FFmpeg frame");
  }

  Frame result;
  internal::CheckFfmpeg(av_frame_ref(result.get(), frame_),
                        "failed to reference an FFmpeg frame");
  return result;
}

void Frame::MakeWritable() {
  if (frame_ == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot make an empty FFmpeg frame writable");
  }
  internal::CheckFfmpeg(av_frame_make_writable(frame_),
                        "failed to make an FFmpeg frame writable");
}

}  // namespace mw::streamer::ffmpeg

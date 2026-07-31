#include "mw/ffmpeg/frame.h"

#include <new>
#include <stdexcept>
#include <utility>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {

Frame::Frame() : frame_(av_frame_alloc()) {
  if (!frame_) {
    throw std::bad_alloc();
  }
}

Frame::Frame(AVFrame* frame) : frame_(frame) {
  if (!frame_) {
    throw std::invalid_argument("不能接管空AVFrame");
  }
}

Frame::~Frame() { av_frame_free(&frame_); }

Frame::Frame(const Frame& other) : Frame() {
  if (!other.frame_) {
    throw std::logic_error("不能引用已移动的Frame");
  }
  ThrowIfError(av_frame_ref(frame_, other.frame_), "引用AVFrame");
}

Frame& Frame::operator=(const Frame& other) {
  if (this != &other) {
    Frame copy(other);
    std::swap(frame_, copy.frame_);
  }
  return *this;
}

Frame::Frame(Frame&& other) noexcept
    : frame_(std::exchange(other.frame_, nullptr)) {}

Frame& Frame::operator=(Frame&& other) noexcept {
  if (this != &other) {
    av_frame_free(&frame_);
    frame_ = std::exchange(other.frame_, nullptr);
  }
  return *this;
}

Frame Frame::Clone(const AVFrame& source) {
  auto* frame = av_frame_clone(&source);
  if (!frame) {
    throw std::bad_alloc();
  }
  return Frame(frame);
}

Frame Frame::Clone() const {
  if (!frame_) {
    throw std::logic_error("不能Clone已移动的Frame");
  }
  return Clone(*frame_);
}

Frame Frame::Ref() const { return Frame(*this); }

void Frame::CopyPropertiesFrom(const Frame& source) {
  if (!frame_ || !source.frame_) {
    throw std::logic_error("不能复制已移动Frame的属性");
  }
  ThrowIfError(av_frame_copy_props(frame_, source.frame_), "复制AVFrame属性");
}

void Frame::ClearCrop() noexcept {
  if (!frame_) {
    return;
  }
  frame_->crop_top = 0;
  frame_->crop_bottom = 0;
  frame_->crop_left = 0;
  frame_->crop_right = 0;
}

const AVFrame* Frame::get() const noexcept { return frame_; }

AVFrame* Frame::get() noexcept { return frame_; }

const AVFrame* Frame::operator->() const noexcept { return frame_; }

AVFrame* Frame::operator->() noexcept { return frame_; }

void Frame::Unref() noexcept {
  if (frame_) {
    av_frame_unref(frame_);
  }
}

}  // namespace mw::streamer::ffmpeg

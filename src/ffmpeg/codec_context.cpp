#include "mw/streamer/ffmpeg/codec_context.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

CodecContext::CodecContext(const AVCodec* codec)
    : context_(avcodec_alloc_context3(codec)) {
  if (context_ == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate FFmpeg codec context");
  }
}

CodecContext::~CodecContext() noexcept { avcodec_free_context(&context_); }

CodecContext::CodecContext(CodecContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

CodecContext& CodecContext::operator=(CodecContext&& other) noexcept {
  if (this != &other) {
    avcodec_free_context(&context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

AVCodecContext* CodecContext::get() noexcept { return context_; }

const AVCodecContext* CodecContext::get() const noexcept { return context_; }

}  // namespace mw::streamer::ffmpeg

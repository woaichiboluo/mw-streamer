#include "mw/ffmpeg/codec_context.h"

#include <new>
#include <utility>

namespace mw::streamer::ffmpeg {

CodecContext::CodecContext(const AVCodec* codec)
    : context_(avcodec_alloc_context3(codec)) {
  if (!context_) {
    throw std::bad_alloc();
  }
}

CodecContext::~CodecContext() { avcodec_free_context(&context_); }

CodecContext::CodecContext(CodecContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

CodecContext& CodecContext::operator=(CodecContext&& other) noexcept {
  if (this != &other) {
    avcodec_free_context(&context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

const AVCodecContext* CodecContext::get() const noexcept { return context_; }

AVCodecContext* CodecContext::get() noexcept { return context_; }

void CodecContext::FlushBuffers() noexcept {
  if (context_) {
    avcodec_flush_buffers(context_);
  }
}

}  // namespace mw::streamer::ffmpeg

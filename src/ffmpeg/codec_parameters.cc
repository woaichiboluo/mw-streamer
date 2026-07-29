#include "mw/ffmpeg/codec_parameters.h"

#include <new>
#include <utility>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {

CodecParameters::CodecParameters() : parameters_(avcodec_parameters_alloc()) {
  if (!parameters_) {
    throw std::bad_alloc();
  }
}

CodecParameters::CodecParameters(const AVCodecParameters& source)
    : CodecParameters() {
  ThrowIfError(avcodec_parameters_copy(parameters_, &source),
               "复制FFmpeg编解码参数");
}

CodecParameters::~CodecParameters() { avcodec_parameters_free(&parameters_); }

CodecParameters::CodecParameters(const CodecParameters& other)
    : CodecParameters() {
  if (!other.parameters_) {
    return;
  }
  ThrowIfError(avcodec_parameters_copy(parameters_, other.parameters_),
               "复制FFmpeg编解码参数");
}

CodecParameters& CodecParameters::operator=(const CodecParameters& other) {
  if (this != &other) {
    CodecParameters copy(other);
    Swap(copy);
  }
  return *this;
}

CodecParameters::CodecParameters(CodecParameters&& other) noexcept
    : parameters_(std::exchange(other.parameters_, nullptr)) {}

CodecParameters& CodecParameters::operator=(CodecParameters&& other) noexcept {
  if (this != &other) {
    avcodec_parameters_free(&parameters_);
    parameters_ = std::exchange(other.parameters_, nullptr);
  }
  return *this;
}

const AVCodecParameters* CodecParameters::get() const noexcept {
  return parameters_;
}

AVCodecParameters* CodecParameters::get() noexcept { return parameters_; }

void CodecParameters::Swap(CodecParameters& other) noexcept {
  std::swap(parameters_, other.parameters_);
}

}  // namespace mw::streamer::ffmpeg

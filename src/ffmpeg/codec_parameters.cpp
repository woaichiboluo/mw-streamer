#include "mw/streamer/ffmpeg/codec_parameters.h"

#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {

CodecParameters::CodecParameters() : parameters_(avcodec_parameters_alloc()) {
  if (parameters_ == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate FFmpeg codec parameters");
  }
}

CodecParameters::~CodecParameters() noexcept {
  avcodec_parameters_free(&parameters_);
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

AVCodecParameters* CodecParameters::get() noexcept { return parameters_; }

const AVCodecParameters* CodecParameters::get() const noexcept {
  return parameters_;
}

CodecParameters CodecParameters::Clone() const {
  if (parameters_ == nullptr) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "cannot clone empty FFmpeg codec parameters");
  }

  CodecParameters result;
  internal::CheckFfmpeg(avcodec_parameters_copy(result.get(), parameters_),
                        "failed to clone FFmpeg codec parameters");
  return result;
}

}  // namespace mw::streamer::ffmpeg

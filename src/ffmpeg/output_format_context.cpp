#include "mw/streamer/ffmpeg/output_format_context.h"

#include <string>
#include <utility>

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::ffmpeg {
namespace {

AVFormatContext* AllocateOutputContext(std::string_view format_name) {
  if (format_name.empty()) {
    internal::ThrowError(StatusCode::kInvalidArgument,
                         "output format name is empty");
  }

  AVFormatContext* context = nullptr;
  const std::string null_terminated_name(format_name);
  const int result = avformat_alloc_output_context2(
      &context, nullptr, null_terminated_name.c_str(), nullptr);
  if (result < 0) {
    avformat_free_context(context);
    internal::ThrowError(StatusCode::kFfmpegError,
                         "failed to allocate an output format context", result);
  }
  if (context == nullptr) {
    internal::ThrowError(StatusCode::kFfmpegError,
                         "FFmpeg returned an empty output format context");
  }
  return context;
}

}  // namespace

OutputFormatContext::OutputFormatContext(std::string_view format_name)
    : context_(AllocateOutputContext(format_name)) {}

OutputFormatContext::~OutputFormatContext() noexcept {
  avformat_free_context(context_);
}

OutputFormatContext::OutputFormatContext(OutputFormatContext&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)) {}

OutputFormatContext& OutputFormatContext::operator=(
    OutputFormatContext&& other) noexcept {
  if (this != &other) {
    avformat_free_context(context_);
    context_ = std::exchange(other.context_, nullptr);
  }
  return *this;
}

AVFormatContext* OutputFormatContext::get() noexcept { return context_; }

const AVFormatContext* OutputFormatContext::get() const noexcept {
  return context_;
}

}  // namespace mw::streamer::ffmpeg

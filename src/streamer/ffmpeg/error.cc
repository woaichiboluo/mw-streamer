#include "mw/streamer/ffmpeg/error.h"

extern "C" {
#include <libavutil/error.h>
}

#include <fmt/format.h>

namespace mw::streamer::ffmpeg {

std::string AvErrorStr(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  if (av_strerror(error, buffer, sizeof(buffer)) < 0) {
    return fmt::format("FFmpeg error {}", error);
  }
  return buffer;
}

FfmpegException::FfmpegException(int error, std::string_view operation)
    : std::runtime_error(
          fmt::format("{}失败: {}", operation, AvErrorStr(error))),
      error_code_(error) {}

int FfmpegException::error_code() const noexcept { return error_code_; }

void FfmpegException::throwIfError(int result, std::string_view operation) {
  if (result < 0) {
    throw FfmpegException(result, operation);
  }
}

}  // namespace mw::streamer::ffmpeg

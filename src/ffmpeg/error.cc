#include "mw/ffmpeg/error.h"

#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
}

#include <fmt/format.h>

namespace mw::streamer::ffmpeg {

std::string ErrorText(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  if (av_strerror(error, buffer, sizeof(buffer)) < 0) {
    return fmt::format("FFmpeg error {}", error);
  }
  return buffer;
}

void ThrowIfError(int result, const char* operation) {
  if (result < 0) {
    throw std::runtime_error(
        fmt::format("{}失败: {}", operation, ErrorText(result)));
  }
}

}  // namespace mw::streamer::ffmpeg

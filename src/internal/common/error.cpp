#include "mw/streamer/internal/common/error.h"

#include <array>
#include <string>
#include <utility>

#include "fmt/format.h"

extern "C" {
#include <libavutil/error.h>
}

namespace mw::streamer::internal {

Error::Error(StatusCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

StatusCode Error::code() const noexcept { return code_; }

[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message) {
  throw Error(code, std::string(error_message));
}

[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message,
                             int ffmpeg_error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(ffmpeg_error, buffer.data(), buffer.size()) < 0) {
    throw Error(
        code, fmt::format("{}: FFmpeg error {}", error_message, ffmpeg_error));
  }
  throw Error(code, fmt::format("{}: {} ({})", error_message, buffer.data(),
                                ffmpeg_error));
}

[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message,
                             const std::error_code& error) {
  throw Error(code, fmt::format("{}: {}", error_message, error.message()));
}

void CheckFfmpeg(int result, std::string_view operation) {
  if (result < 0) {
    ThrowError(StatusCode::kFfmpegError, operation, result);
  }
}

}  // namespace mw::streamer::internal

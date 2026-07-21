#ifndef MW_STREAMER_INTERNAL_COMMON_ERROR_H_
#define MW_STREAMER_INTERNAL_COMMON_ERROR_H_

#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "mw/streamer/status.h"

namespace mw::streamer::internal {

class Error final : public std::runtime_error {
 public:
  Error(StatusCode code, std::string message);

  [[nodiscard]] StatusCode code() const noexcept;

 private:
  StatusCode code_;
};

[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message);
[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message,
                             int ffmpeg_error);
[[noreturn]] void ThrowError(StatusCode code, std::string_view error_message,
                             const std::error_code& error);

void CheckFfmpeg(int result, std::string_view operation);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_ERROR_H_

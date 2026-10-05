#ifndef MW_STREAMER_FFMPEG_ERROR_H_
#define MW_STREAMER_FFMPEG_ERROR_H_

#include <stdexcept>
#include <string>
#include <string_view>

#include "mw/export.h"

namespace mw::streamer::ffmpeg {

MW_STREAMER_API std::string AvErrorStr(int error);

class MW_STREAMER_API FfmpegException : public std::runtime_error {
 public:
  FfmpegException(int error, std::string_view operation);

  int error_code() const noexcept;
  static void throwIfError(int result, std::string_view operation);

 private:
  int error_code_;
};

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_FFMPEG_ERROR_H_

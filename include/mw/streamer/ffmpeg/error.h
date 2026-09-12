#ifndef MW_STREAMER_FFMPEG_ERROR_H_
#define MW_STREAMER_FFMPEG_ERROR_H_

#include <string>

namespace mw::streamer {

std::string ErrorText(int error);
void ThrowIfError(int result, const char* operation);

}  // namespace mw::streamer

#endif  // MW_STREAMER_FFMPEG_ERROR_H_

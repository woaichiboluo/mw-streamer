#ifndef MW_STREAMER_INCLUDE_MW_FFMPEG_ERROR_H_
#define MW_STREAMER_INCLUDE_MW_FFMPEG_ERROR_H_

#include <string>

namespace mw::streamer::ffmpeg {

std::string ErrorText(int error);
void ThrowIfError(int result, const char* operation);

}  // namespace mw::streamer::ffmpeg

#endif  // MW_STREAMER_INCLUDE_MW_FFMPEG_ERROR_H_

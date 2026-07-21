#ifndef MW_STREAMER_INTERNAL_COMMON_FFMPEG_LOG_H_
#define MW_STREAMER_INTERNAL_COMMON_FFMPEG_LOG_H_

#include "spdlog/common.h"

namespace mw::streamer::internal {

[[nodiscard]] spdlog::level::level_enum MapFfmpegLogLevel(
    int ffmpeg_level) noexcept;

void InstallFfmpegLogBridge() noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_COMMON_FFMPEG_LOG_H_

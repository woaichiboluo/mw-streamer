#ifndef MW_STREAMER_INTERNAL_INPUT_OPTIONS_H_
#define MW_STREAMER_INTERNAL_INPUT_OPTIONS_H_

#include "mw/streamer/internal/common/ffmpeg_interrupt.h"
#include "mw/streamer/internal/demuxer.h"
#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {

[[nodiscard]] DemuxerOpenOptions MakeDemuxerOpenOptions(
    const InputConfig& config, FfmpegInterrupt* interrupt);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_INPUT_OPTIONS_H_

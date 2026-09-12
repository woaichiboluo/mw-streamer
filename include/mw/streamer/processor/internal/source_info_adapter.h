#ifndef MW_STREAMER_PROCESSOR_INTERNAL_SOURCE_INFO_ADAPTER_H_
#define MW_STREAMER_PROCESSOR_INTERNAL_SOURCE_INFO_ADAPTER_H_

#include <optional>

#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/processor/processor.h"

namespace mw::streamer::internal {

MwStreamerProcessorSourceInfo MakeProcessorSourceInfo(
    const std::optional<StreamInfo>& audio_stream,
    const std::optional<StreamInfo>& video_stream);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PROCESSOR_INTERNAL_SOURCE_INFO_ADAPTER_H_

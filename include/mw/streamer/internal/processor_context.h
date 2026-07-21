#ifndef MW_STREAMER_INTERNAL_PROCESSOR_CONTEXT_H_
#define MW_STREAMER_INTERNAL_PROCESSOR_CONTEXT_H_

#include <cstddef>
#include <vector>

#include "mw/streamer/internal/stream_info.h"
#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/processor.h"

namespace mw::streamer::internal {

[[nodiscard]] MwInputSourceInfo MakeInputSourceInfo(
    std::size_t input_index, const StreamInfo& video_stream,
    const StreamInfo* audio_stream);

[[nodiscard]] MwProcessorContext MakeProcessorContext(
    int device_id, const std::vector<MwInputSourceInfo>& input_sources,
    std::size_t primary_video_input_index,
    const VideoEncoderConfig& video_encoder, bool has_audio);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_PROCESSOR_CONTEXT_H_

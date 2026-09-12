#ifndef MW_STREAMER_PIPELINE_INTERNAL_PIPELINE_BUILDER_H_
#define MW_STREAMER_PIPELINE_INTERNAL_PIPELINE_BUILDER_H_

#include <memory>

#include "mw/streamer/init/internal/runtime.h"
#include "mw/streamer/pipeline/pipeline_builder.h"

namespace mw::streamer::internal {

std::unique_ptr<Pipeline> BuildPipelineWithRuntime(
    const PipelineConfig& config, const ProcessorBindings& bindings,
    const RuntimeConfig& runtime);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PIPELINE_INTERNAL_PIPELINE_BUILDER_H_

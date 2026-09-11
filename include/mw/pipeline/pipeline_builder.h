#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_BUILDER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_BUILDER_H_

#include <map>
#include <memory>
#include <string>

#include "mw/pipeline/pipeline.h"
#include "mw/pipeline/pipeline_config.h"
#include "mw/processor/processor.h"

namespace mw::streamer::pipeline {

// Optional bindings by Sink ID, supplied by the host and never serialized.
// Unspecified callbacks retain each Processor's existing passthrough/ignore
// semantics. Callback contexts are borrowed until the built Pipeline stops.
struct ProcessorBindings {
  std::map<std::string, MwStreamerAnalysisProcessorCallbacks> analysis;
  std::map<std::string, MwStreamerTransformProcessorCallbacks> transform;
};

// Validates and constructs an exclusively owned tree, with message routes
// bound by ID. Does not start the Pipeline or borrow the configuration object.
// Unknown/mismatched callback bindings are configuration errors.
std::unique_ptr<Pipeline> BuildPipeline(const PipelineConfig& config,
                                        const ProcessorBindings& bindings = {});

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_BUILDER_H_

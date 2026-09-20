#ifndef MW_STREAMER_PIPELINE_PIPELINE_BUILDER_H_
#define MW_STREAMER_PIPELINE_PIPELINE_BUILDER_H_

#include <map>
#include <memory>
#include <string>

#include "mw/streamer/pipeline/pipeline.h"
#include "mw/streamer/pipeline/pipeline_config.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/sink/frame_custom_sink.h"
#include "mw/streamer/sink/packet_custom_sink.h"

namespace mw::streamer {

// Optional bindings by Sink ID, supplied by the host and never serialized.
// Unspecified callbacks retain each Processor's existing passthrough/ignore
// semantics. Callback contexts are borrowed until the built Pipeline stops.
struct ProcessorBindings {
  std::map<std::string, MwStreamerAnalysisProcessorCallbacks> analysis;
  std::map<std::string, MwStreamerTransformProcessorCallbacks> transform;
  // Each Custom Sink requires a binding of the matching media type. Callback
  // bindings are supplied by the host and never serialized into TOML.
  std::map<std::string, MwStreamerFrameCustomSinkCallbacks> frame_custom_sinks;
  std::map<std::string, MwStreamerPacketCustomSinkCallbacks>
      packet_custom_sinks;
};

// Preferred name now that the collection also contains Custom Sink bindings.
// ProcessorBindings remains source-compatible for existing callers.
using PipelineBindings = ProcessorBindings;

// Validates and constructs an exclusively owned tree. Does not start the
// Pipeline or borrow the configuration object.
// Unknown/mismatched callback bindings are configuration errors.
std::unique_ptr<Pipeline> BuildPipeline(const PipelineConfig& config,
                                        const ProcessorBindings& bindings = {});

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_PIPELINE_BUILDER_H_

#ifndef MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_
#define MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "mw/pipeline/pipeline_builder.h"
#include "mw/pipeline/pipeline_config.h"

namespace mw::streamer::config {

// Unified Pipeline format. Both directions validate configuration. Formatting
// and comments are not preserved. Processor business configuration is supplied
// separately through Pipeline::SetProcessorConfig.
// String parsing preserves paths. File loading resolves local paths against
// the source file's directory; URLs and empty optional paths stay unchanged.
pipeline::PipelineConfig ParsePipelineConfigFromToml(std::string_view text);
std::string SerializePipelineConfigToToml(
    const pipeline::PipelineConfig& config);
pipeline::PipelineConfig LoadPipelineConfigFromToml(
    const std::filesystem::path& path);
void SavePipelineConfigToToml(const pipeline::PipelineConfig& config,
                              const std::filesystem::path& path);

// Builds a Pipeline from one streamer TOML document. Its optional [log] and
// [zlm] sections configure the process runtime before any media object is
// created. The returned Pipeline is not started.
std::unique_ptr<pipeline::Pipeline> BuildPipelineFromToml(
    const std::filesystem::path& path,
    const pipeline::ProcessorBindings& bindings = {});

}  // namespace mw::streamer::config

#endif  // MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

#ifndef MW_STREAMER_CONFIG_TOML_H_
#define MW_STREAMER_CONFIG_TOML_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "mw/streamer/pipeline/pipeline_builder.h"
#include "mw/streamer/pipeline/pipeline_config.h"

namespace mw::streamer {

// Unified Pipeline format. Both directions validate configuration. Formatting
// and comments are not preserved. Processor business configuration is supplied
// separately through Pipeline::SetProcessorConfig.
// String parsing preserves paths. File loading resolves local paths against
// the source file's directory; URLs and empty optional paths stay unchanged.
PipelineConfig ParsePipelineConfigFromToml(std::string_view text);
std::string SerializePipelineConfigToToml(
    const PipelineConfig& config);
PipelineConfig LoadPipelineConfigFromToml(
    const std::filesystem::path& path);
void SavePipelineConfigToToml(const PipelineConfig& config,
                              const std::filesystem::path& path);

// Builds a Pipeline from one streamer TOML document. Its optional [log] and
// [zlm] sections configure the process runtime before any media object is
// created. The returned Pipeline is not started.
std::unique_ptr<Pipeline> BuildPipelineFromToml(
    const std::filesystem::path& path,
    const ProcessorBindings& bindings = {});

}  // namespace mw::streamer

#endif  // MW_STREAMER_CONFIG_TOML_H_

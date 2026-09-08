#ifndef MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_
#define MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "mw/init/init.h"
#include "mw/pipeline/pipeline_config.h"

namespace mw::streamer::config {

// Unified Pipeline format. Both directions validate configuration. Formatting
// and comments are not preserved; processor.config remains a TOML table.
// String parsing preserves paths. File loading resolves local paths against
// the source file's directory; URLs and empty optional paths stay unchanged.
pipeline::PipelineConfig ParsePipelineConfigFromToml(std::string_view text);
std::string SerializePipelineConfigToToml(
    const pipeline::PipelineConfig& config);
pipeline::PipelineConfig LoadPipelineConfigFromToml(
    const std::filesystem::path& path);
void SavePipelineConfigToToml(const pipeline::PipelineConfig& config,
                              const std::filesystem::path& path);

// Loads one configuration object from a TOML document. Missing fields retain
// their C++ defaults. Unknown fields, invalid types, and integer values outside
// the destination C++ type are rejected. Semantic validation remains owned by
// the component that consumes the resulting config. The TOML implementation
// is intentionally not exposed by this public API.
InitConfig LoadInitConfigFromToml(const std::filesystem::path& path);

}  // namespace mw::streamer::config

#endif  // MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

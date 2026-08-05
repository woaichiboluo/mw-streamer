#ifndef MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_
#define MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

#include <filesystem>

#include "mw/init/init.h"
#include "mw/pipeline/config.h"

namespace mw::streamer::config {

// Loads one configuration object from a TOML document. Missing fields retain
// their C++ defaults. Unknown fields, invalid types, and integer values outside
// the destination C++ type are rejected. Semantic validation remains owned by
// the component that consumes the resulting config. The TOML implementation
// is intentionally not exposed by this public API.
InitConfig LoadInitConfigFromToml(const std::filesystem::path& path);
pipeline::StreamingPipelineConfig LoadStreamingPipelineConfigFromToml(
    const std::filesystem::path& path);
pipeline::RemuxPipelineConfig LoadRemuxPipelineConfigFromToml(
    const std::filesystem::path& path);
pipeline::LocalFilePipelineConfig LoadFilePipelineConfigFromToml(
    const std::filesystem::path& path);

}  // namespace mw::streamer::config

#endif  // MW_STREAMER_INCLUDE_MW_CONFIG_TOML_H_

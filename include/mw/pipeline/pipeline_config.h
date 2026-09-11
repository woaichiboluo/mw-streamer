#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_CONFIG_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mw/decoder/config.h"
#include "mw/encoder/config.h"
#include "mw/input/config.h"
#include "mw/output/config.h"
#include "mw/synchronizer/config.h"

namespace mw::streamer::pipeline {

enum class InputType { kZlm, kFile };
enum class SinkType {
  kDecoder,
  kAnalysisProcessor,
  kTransformProcessor,
  kSynchronizer,
  kEncoder,
  kRemux,
};

struct InputConfig {
  InputType type = InputType::kZlm;
  input::ZlmInputConfig options;
  input::FileInputConfig file;
  std::vector<std::string> downstream;
};

// Owns configuration only, with no running sinks or callbacks. Each concrete
// node retains its existing typed options; its type cannot disagree with them.
struct SinkConfig {
  explicit SinkConfig(std::string node_id) : id(std::move(node_id)) {}
  virtual ~SinkConfig() = default;
  virtual SinkType type() const noexcept = 0;

  std::string id;
  std::vector<std::string> downstream;
  // Empty means unbound. Message edges do not participate in media ownership.
  std::string message_receiver;
};

struct DecoderNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override { return SinkType::kDecoder; }
  decoder::DecoderSinkConfig options;
};

struct AnalysisProcessorNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override {
    return SinkType::kAnalysisProcessor;
  }
};

struct TransformProcessorNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override {
    return SinkType::kTransformProcessor;
  }
};

struct SynchronizerNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override { return SinkType::kSynchronizer; }
  synchronizer::SynchronizerSinkConfig options;
};

struct EncoderNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override { return SinkType::kEncoder; }
  encoder::EncoderSinkConfig options;
};

struct RemuxNodeConfig final : SinkConfig {
  using SinkConfig::SinkConfig;
  SinkType type() const noexcept override { return SinkType::kRemux; }
  output::RemuxSinkConfig options;
};

// Move-only, exclusively owns all node descriptions in declaration order.
// Downstream arrays determine delivery order; declarations may be forward
// referenced. Relative paths in a manually populated config use the caller's
// working directory. File loading resolves paths against the TOML directory.
struct PipelineConfig {
  InputConfig input;
  std::vector<std::unique_ptr<SinkConfig>> sinks;
};

// Validates IDs, reachability, unique media ownership, types and parameters
// without starting inputs, allocating workers, or opening output resources.
// Runtime-only properties (actual tracks, frames and codec availability) are
// still checked by their owning components.
void ValidatePipelineConfig(const PipelineConfig& config);

}  // namespace mw::streamer::pipeline

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_PIPELINE_CONFIG_H_

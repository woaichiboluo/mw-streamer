#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_

#include <cstdint>
#include <string>

namespace mw::streamer::processor {

struct StreamingProcessorConfig {
  // Fixed for the Processor lifetime. Both fields must be zero when streaming
  // audio without video.
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;
  std::string config;
};

struct FileProcessorConfig {
  std::string config;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_

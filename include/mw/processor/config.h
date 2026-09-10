#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_

#include <string>

namespace mw::streamer::processor {

struct StreamingProcessorConfig {
  std::string config;
};

struct FileProcessorConfig {
  std::string config;
};

}  // namespace mw::streamer::processor

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_CONFIG_H_

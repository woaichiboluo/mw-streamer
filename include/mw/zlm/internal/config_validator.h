#ifndef MW_STREAMER_INCLUDE_MW_ZLM_INTERNAL_CONFIG_VALIDATOR_H_
#define MW_STREAMER_INCLUDE_MW_ZLM_INTERNAL_CONFIG_VALIDATOR_H_

#include "mw/zlm/config.h"

namespace mw::streamer::zlm::internal {

void ValidatePlayerConfig(const PlayerConfig& config);
void ValidateRecordingConfig(const RecordingConfig& config);
void ValidateOutputConfig(const OutputConfig& config);

}  // namespace mw::streamer::zlm::internal

#endif  // MW_STREAMER_INCLUDE_MW_ZLM_INTERNAL_CONFIG_VALIDATOR_H_

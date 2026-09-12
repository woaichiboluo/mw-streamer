#ifndef MW_STREAMER_ZLM_INTERNAL_CONFIG_VALIDATOR_H_
#define MW_STREAMER_ZLM_INTERNAL_CONFIG_VALIDATOR_H_

#include "mw/streamer/zlm/config.h"

namespace mw::streamer::internal {

void ValidatePlayerConfig(const PlayerConfig& config);
void ValidateRecordingConfig(const RecordingConfig& config);
void ValidateOutputConfig(const OutputConfig& config);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_ZLM_INTERNAL_CONFIG_VALIDATOR_H_

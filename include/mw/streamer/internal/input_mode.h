#ifndef MW_STREAMER_INTERNAL_INPUT_MODE_H_
#define MW_STREAMER_INTERNAL_INPUT_MODE_H_

#include "mw/streamer/pipeline_config.h"

namespace mw::streamer::internal {

[[nodiscard]] bool IsLiveInput(const InputConfig& input) noexcept;

[[nodiscard]] bool IsFiniteInput(const InputConfig& input) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INTERNAL_INPUT_MODE_H_

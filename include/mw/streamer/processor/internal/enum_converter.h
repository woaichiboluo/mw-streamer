#ifndef MW_STREAMER_PROCESSOR_INTERNAL_ENUM_CONVERTER_H_
#define MW_STREAMER_PROCESSOR_INTERNAL_ENUM_CONVERTER_H_

#include <optional>
#include <string_view>

#include "mw/streamer/processor/processor.h"

namespace mw::streamer::internal {

std::string_view ToName(MwStreamerProcessorBoundaryReason reason) noexcept;
std::optional<MwStreamerProcessorBoundaryReason> ProcessorBoundaryFromName(
    std::string_view name) noexcept;

std::string_view ToName(MwStreamerExecutionType type) noexcept;
std::optional<MwStreamerExecutionType> ExecutionTypeFromName(
    std::string_view name) noexcept;

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_PROCESSOR_INTERNAL_ENUM_CONVERTER_H_

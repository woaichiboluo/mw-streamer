#include "mw/processor/internal/enum_converter.h"

#include <array>
#include <utility>

namespace mw::streamer::processor::internal {
namespace {

template <typename Enum, std::size_t Size>
std::string_view FindName(Enum value,
                          const std::array<std::pair<Enum, std::string_view>,
                                           Size>& mappings) noexcept {
  for (const auto& [candidate, name] : mappings) {
    if (candidate == value) {
      return name;
    }
  }
  return "unknown";
}

template <typename Enum, std::size_t Size>
std::optional<Enum> FindValue(
    std::string_view name,
    const std::array<std::pair<Enum, std::string_view>, Size>&
        mappings) noexcept {
  for (const auto& [value, candidate] : mappings) {
    if (candidate == name) {
      return value;
    }
  }
  return std::nullopt;
}

constexpr std::array kProcessorBoundaries{
    std::pair{kMwStreamerProcessorTimelineReset,
              std::string_view{"timeline_reset"}},
    std::pair{kMwStreamerProcessorEndOfInput, std::string_view{"end_of_input"}},
};

constexpr std::array kExecutionTypes{
    std::pair{kMwStreamerExecutionCpu, std::string_view{"cpu"}},
    std::pair{kMwStreamerExecutionCuda, std::string_view{"cuda"}},
};

}  // namespace

std::string_view ToName(MwStreamerProcessorBoundaryReason reason) noexcept {
  return FindName(reason, kProcessorBoundaries);
}

std::optional<MwStreamerProcessorBoundaryReason> ProcessorBoundaryFromName(
    std::string_view name) noexcept {
  return FindValue(name, kProcessorBoundaries);
}

std::string_view ToName(MwStreamerExecutionType type) noexcept {
  return FindName(type, kExecutionTypes);
}

std::optional<MwStreamerExecutionType> ExecutionTypeFromName(
    std::string_view name) noexcept {
  return FindValue(name, kExecutionTypes);
}

}  // namespace mw::streamer::processor::internal

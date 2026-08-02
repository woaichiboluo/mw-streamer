#include "mw/input/internal/zlm_timestamp_reviser.h"

#include <cctype>
#include <memory>

#include "Common/MediaSource.h"
#include "Common/Stamp.h"
#include "Extension/Frame.h"

namespace mw::streamer::input::internal {

bool ShouldReviseZlmTimestamps(std::string_view url) noexcept {
  const auto separator = url.find("://");
  if (separator == std::string_view::npos) {
    return false;
  }
  const auto scheme = url.substr(0, separator);
  if (scheme.size() != 4 && scheme.size() != 5) {
    return false;
  }
  constexpr std::string_view kRtsps = "rtsps";
  for (std::size_t index = 0; index < scheme.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(scheme[index])) !=
        kRtsps[index]) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<mediakit::Frame> ReviseZlmFrameTimestamp(
    std::shared_ptr<mediakit::Frame> frame, mediakit::Stamp* stamp) {
  if (!stamp) {
    return frame;
  }
  return std::make_shared<mediakit::FrameStamp>(
      std::move(frame), *stamp, mediakit::ProtocolOption::kModifyStampRelative);
}

}  // namespace mw::streamer::input::internal

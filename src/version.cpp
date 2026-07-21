#include "mw/streamer/version.h"

namespace mw::streamer {

std::string_view VersionString() noexcept { return MW_STREAMER_VERSION; }

}  // namespace mw::streamer

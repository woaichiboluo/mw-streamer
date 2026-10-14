#include <signal.h>

#include <cerrno>
#include <system_error>

#include "mw/streamer/platform/runtime.h"

namespace mw::streamer::internal {

void IgnoreSigpipe() {
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPIPE, &action, nullptr) != 0) {
    throw std::system_error(errno, std::generic_category(), "忽略SIGPIPE失败");
  }
}

void EnableUtf8Console() {}

ScopedTimerResolution::ScopedTimerResolution() = default;
ScopedTimerResolution::~ScopedTimerResolution() = default;

}  // namespace mw::streamer::internal

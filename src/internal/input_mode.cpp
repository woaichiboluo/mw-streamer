#include "mw/streamer/internal/input_mode.h"

namespace mw::streamer::internal {

bool IsLiveInput(const InputConfig& input) noexcept {
  switch (input.protocol) {
    case InputProtocol::kRtmp:
    case InputProtocol::kRtsp:
    case InputProtocol::kSrt:
      return true;
    case InputProtocol::kHls:
      return input.hls_mode == HlsMode::kLive;
    case InputProtocol::kFile:
      return false;
  }
  return false;
}

bool IsFiniteInput(const InputConfig& input) noexcept {
  switch (input.protocol) {
    case InputProtocol::kFile:
      return true;
    case InputProtocol::kHls:
      return input.hls_mode == HlsMode::kVod;
    case InputProtocol::kRtmp:
    case InputProtocol::kRtsp:
    case InputProtocol::kSrt:
      return false;
  }
  return false;
}

}  // namespace mw::streamer::internal

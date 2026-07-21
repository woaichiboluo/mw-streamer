#include "mw/streamer/internal/input_options.h"

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {

DemuxerOpenOptions MakeDemuxerOpenOptions(const InputConfig& config,
                                          FfmpegInterrupt* interrupt) {
  if (interrupt == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "input demuxer requires an interrupt controller");
  }

  DemuxerOpenOptions result;
  result.options = config.options;
  result.interrupt = interrupt;
  result.open_timeout = config.open_timeout;
  result.read_timeout = config.read_timeout;
  if (config.protocol == InputProtocol::kRtsp) {
    result.options["rtsp_transport"] =
        config.rtsp_transport == RtspTransport::kTcp ? "tcp" : "udp";
  }
  return result;
}

}  // namespace mw::streamer::internal

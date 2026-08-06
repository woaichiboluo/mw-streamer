#include "mw/processor/internal/execution_context_adapter.h"

#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "mw/ffmpeg/hardware_context.h"

namespace mw::streamer::processor::internal {

MwStreamerExecutionContext MakeProcessorExecutionContext(
    const ffmpeg::HardwareContext* hardware_context) {
  if (!hardware_context) {
    return {kMwStreamerExecutionCpu};
  }

  switch (hardware_context->type()) {
    case AV_HWDEVICE_TYPE_CUDA:
      return {kMwStreamerExecutionCuda};
    default:
      throw std::invalid_argument("Processor暂不支持该硬件执行上下文");
  }
}

}  // namespace mw::streamer::processor::internal

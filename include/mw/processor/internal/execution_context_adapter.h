#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_

#include "mw/processor/processor.h"

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::processor::internal {

MwStreamerExecutionContext MakeProcessorExecutionContext(
    const ffmpeg::HardwareContext* hardware_context);

}  // namespace mw::streamer::processor::internal

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_

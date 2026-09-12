#ifndef MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_
#define MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_

#include "mw/processor/processor.h"

namespace mw::streamer {
class HardwareContext;
}

namespace mw::streamer::internal {

MwStreamerExecutionContext MakeProcessorExecutionContext(
    const HardwareContext* hardware_context);

}  // namespace mw::streamer::internal

#endif  // MW_STREAMER_INCLUDE_MW_PROCESSOR_INTERNAL_EXECUTION_CONTEXT_ADAPTER_H_

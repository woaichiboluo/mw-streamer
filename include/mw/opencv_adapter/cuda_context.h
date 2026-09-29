#ifndef MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_
#define MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_

#include <cuda.h>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

// Returns the borrowed CUDA context owned by FFmpeg. The context follows the
// execution context lifetime, must not be destroyed by the caller, and is not
// made current by this function.
MW_OPENCV_ADAPTER_API CUcontext
GetCudaContext(const MwStreamerExecutionContext& execution);

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_

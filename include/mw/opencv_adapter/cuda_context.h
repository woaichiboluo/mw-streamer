#ifndef MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_
#define MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_

#include <cuda.h>

#include "mw/export.h"
#include "mw/streamer/processor/processor.h"

namespace mw::opencv_adapter {

class MW_OPENCV_ADAPTER_API ScopedCudaContext final {
 public:
  explicit ScopedCudaContext(CUcontext context);
  explicit ScopedCudaContext(const MwStreamerExecutionContext& execution);
  explicit ScopedCudaContext(const MwStreamerVideoBufferView& buffer);
  ~ScopedCudaContext();

  ScopedCudaContext(const ScopedCudaContext&) = delete;
  ScopedCudaContext& operator=(const ScopedCudaContext&) = delete;
  ScopedCudaContext(ScopedCudaContext&&) = delete;
  ScopedCudaContext& operator=(ScopedCudaContext&&) = delete;

 private:
  bool pushed_ = false;
};

// Returns the borrowed CUDA context owned by FFmpeg. The context follows the
// execution context lifetime, must not be destroyed by the caller, and is not
// made current by this function.
MW_OPENCV_ADAPTER_API CUcontext
GetCudaContext(const MwStreamerExecutionContext& execution);

MW_OPENCV_ADAPTER_API CUcontext
GetCudaContext(const MwStreamerVideoBufferView& buffer);

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_CUDA_CONTEXT_H_

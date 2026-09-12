#ifndef MW_OPENCV_ADAPTER_INTERNAL_CUDA_DRIVER_H_
#define MW_OPENCV_ADAPTER_INTERNAL_CUDA_DRIVER_H_

namespace mw::opencv_adapter {

// Initializes the process-wide CUDA Driver API once. A failed initialization
// may be retried by a later call.
void EnsureCudaDriverInitialized();

}  // namespace mw::opencv_adapter

#endif  // MW_OPENCV_ADAPTER_INTERNAL_CUDA_DRIVER_H_

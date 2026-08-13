#include "cuda_driver.h"

#include <cuda.h>

#include <mutex>
#include <stdexcept>
#include <string>

namespace mw::streamer::opencv_adapter::internal {
namespace {

void InitializeCudaDriver() {
  const CUresult result = cuInit(0);
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* error_name = nullptr;
  cuGetErrorName(result, &error_name);
  throw std::runtime_error(std::string("初始化CUDA驱动失败: ") +
                           (error_name ? error_name : "CUDA_ERROR_UNKNOWN"));
}

}  // namespace

void EnsureCudaDriverInitialized() {
  static std::once_flag once;
  std::call_once(once, InitializeCudaDriver);
}

}  // namespace mw::streamer::opencv_adapter::internal

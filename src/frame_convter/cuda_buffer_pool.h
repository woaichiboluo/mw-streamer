#ifndef MW_SRC_FRAME_CONVTER_CUDA_BUFFER_POOL_H_
#define MW_SRC_FRAME_CONVTER_CUDA_BUFFER_POOL_H_

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <opencv2/core/cuda.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "mw/log.h"

namespace mw::frame_convter::internal {

inline void CheckCuda(cudaError_t status) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string("CUDA 帧转换失败: ") +
                             cudaGetErrorString(status));
  }
}

inline void CheckDriver(CUresult status) {
  if (status != CUDA_SUCCESS) {
    const char* message = nullptr;
    cuGetErrorString(status, &message);
    throw std::runtime_error(std::string("CUDA 驱动操作失败: ") +
                             (message ? message : "未知错误"));
  }
}

class ContextScope final {
 public:
  explicit ContextScope(CUcontext context) {
    CheckDriver(cuCtxPushCurrent(context));
  }
  ~ContextScope() {
    if (active_) {
      CUcontext previous = nullptr;
      cuCtxPopCurrent(&previous);
    }
  }
  void Finish() {
    CUcontext previous = nullptr;
    const auto status = cuCtxPopCurrent(&previous);
    active_ = false;
    CheckDriver(status);
  }
  ContextScope(const ContextScope&) = delete;
  ContextScope& operator=(const ContextScope&) = delete;

 private:
  bool active_ = true;
};

// Fixed-size blocks avoid fragmentation and per-frame allocation of CPU
// bookkeeping. Slot addresses remain stable, including their refcounts.
class GpuMatBufferPool final : public cv::cuda::GpuMat::Allocator {
 public:
  GpuMatBufferPool(CUcontext context, std::size_t capacity)
      : context_(context), capacity_(capacity) {
    if (capacity_ == 0) return;
    ContextScope binding(context_);
    CheckCuda(cudaMalloc(reinterpret_cast<void**>(&data_), capacity_));
  }

  ~GpuMatBufferPool() override {
    if (!data_) return;
    if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) {
      MW_LOG_ERROR("frame_convter", "无法绑定 CUDA 上下文以释放 GpuMat 池");
      return;
    }
    if (cudaFree(data_) != cudaSuccess) {
      MW_LOG_ERROR("frame_convter", "释放 GpuMat 池显存失败");
    }
    CUcontext previous = nullptr;
    if (cuCtxPopCurrent(&previous) != CUDA_SUCCESS) {
      MW_LOG_ERROR("frame_convter", "释放 GpuMat 池后恢复 CUDA 上下文失败");
    }
  }

  bool allocate(cv::cuda::GpuMat* image, int rows, int cols,
                std::size_t element_size) override {
    const auto pitch = (static_cast<std::size_t>(cols) * element_size + 255U) &
                       ~std::size_t(255U);
    std::unique_lock<std::mutex> lock(mutex_);
    if (pitch != 0 && static_cast<std::size_t>(rows) <= capacity_ / pitch) {
      const auto bytes = pitch * static_cast<std::size_t>(rows);
      if (slots_.empty()) {
        // Complete the only throwing metadata allocation before committing
        // the shape. No allocation occurs when returning a block.
        slots_.resize(capacity_ / bytes);
        for (std::size_t i = 0; i < slots_.size(); ++i) slots_[i].next = i + 1;
        rows_ = rows;
        cols_ = cols;
        element_size_ = element_size;
        block_bytes_ = bytes;
      }
      if (rows == rows_ && cols == cols_ && element_size == element_size_ &&
          free_head_ < slots_.size()) {
        const auto index = free_head_;
        auto& slot = slots_[index];
        free_head_ = slot.next;
        image->data = data_ + index * block_bytes_;
        image->step = pitch;
        image->refcount = &slot.refcount;
        return true;
      }
    }
    lock.unlock();
    // Explicitly transfer ownership to the standard allocator. Returning
    // false would instead select the process's configurable default allocator.
    auto* standard = cv::cuda::GpuMat::getStdAllocator();
    const bool allocated = standard->allocate(image, rows, cols, element_size);
    if (allocated) image->allocator = standard;
    return allocated;
  }

  void free(cv::cuda::GpuMat* image) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    // ROI data may be offset; datastart identifies the original whole block.
    const auto index =
        static_cast<std::size_t>(image->datastart - data_) / block_bytes_;
    if (slots_[index].retired) return;
    slots_[index].next = free_head_;
    free_head_ = index;
  }

  void Retire(const cv::cuda::GpuMat& image) {
    if (image.allocator != this) return;
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto index =
        static_cast<std::size_t>(image.datastart - data_) / block_bytes_;
    // A failed submission can have queued partial work. Do not lend this
    // unreturned output again; callers still drain errors before destruction.
    slots_[index].retired = true;
  }

 private:
  struct Slot {
    int refcount = 0;
    std::size_t next = 0;
    bool retired = false;
  };
  CUcontext context_;
  std::size_t capacity_;
  std::uint8_t* data_ = nullptr;
  std::mutex mutex_;
  std::vector<Slot> slots_;
  std::size_t free_head_ = 0;
  std::size_t block_bytes_ = 0;
  std::size_t element_size_ = 0;
  int rows_ = 0;
  int cols_ = 0;
};

}  // namespace mw::frame_convter::internal

#endif  // MW_SRC_FRAME_CONVTER_CUDA_BUFFER_POOL_H_

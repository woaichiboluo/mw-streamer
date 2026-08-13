#include "mw/opencv_adapter/cuda_frame.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "cuda_driver.h"

namespace mw::streamer::opencv_adapter {
namespace {

using internal::EnsureCudaDriverInitialized;

constexpr std::size_t kMaxPlaneCount = 4;

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* error_name = nullptr;
  cuGetErrorName(result, &error_name);
  throw std::runtime_error(std::string(operation) + "失败: " +
                           (error_name ? error_name : "CUDA_ERROR_UNKNOWN"));
}

void ThrowIfCudaRuntimeError(cudaError_t result, const char* operation) {
  if (result == cudaSuccess) {
    return;
  }
  throw std::runtime_error(std::string(operation) +
                           "失败: " + cudaGetErrorName(result));
}

void ValidateSource(const MwStreamerVideoFrameView& source) {
  const auto& buffer = source.buffer;
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("CudaFrame只支持linear视频存储");
  }
  if (buffer.memory_type != kMwStreamerMemoryHost &&
      buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("CudaFrame收到未知视频内存类型");
  }
  if (buffer.pixel_format == kMwStreamerVideoPixelFormatUnknown) {
    throw std::invalid_argument("CudaFrame不能复制未知视频像素格式");
  }
  if (buffer.width == 0 || buffer.height == 0) {
    throw std::invalid_argument("CudaFrame源视频缺少有效宽高");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes || linear.plane_count == 0 ||
      linear.plane_count > kMaxPlaneCount) {
    throw std::invalid_argument("CudaFrame源视频平面数量无效");
  }
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    const auto stride = static_cast<std::int64_t>(plane.stride_bytes);
    const auto absolute_stride = stride < 0 ? -stride : stride;
    if (plane.address == 0 || plane.stride_bytes == 0 || plane.row_bytes == 0 ||
        plane.row_count == 0 || absolute_stride < plane.row_bytes) {
      throw std::invalid_argument("CudaFrame源视频平面无效: plane=" +
                                  std::to_string(index));
    }
    if (buffer.memory_type == kMwStreamerMemoryCuda && stride < 0) {
      throw std::invalid_argument("CudaFrame不支持负stride的CUDA视频平面");
    }
  }
}

class ScopedCudaContext final {
 public:
  explicit ScopedCudaContext(CUcontext context) {
    ThrowIfCudaError(cuCtxPushCurrent(context), "设置CUDA context");
    pushed_ = true;
  }

  ~ScopedCudaContext() {
    if (pushed_) {
      CUcontext popped_context = nullptr;
      cuCtxPopCurrent(&popped_context);
    }
  }

  ScopedCudaContext(const ScopedCudaContext&) = delete;
  ScopedCudaContext& operator=(const ScopedCudaContext&) = delete;

 private:
  bool pushed_ = false;
};

CUcontext GetPointerContext(std::uintptr_t address) {
  CUcontext context = nullptr;
  ThrowIfCudaError(cuPointerGetAttribute(&context, CU_POINTER_ATTRIBUTE_CONTEXT,
                                         static_cast<CUdeviceptr>(address)),
                   "查询CUDA视频平面context");
  if (!context) {
    throw std::runtime_error("CUDA视频平面没有有效context");
  }
  return context;
}

void ValidateDestination(const MwStreamerVideoFrameView& source,
                         const MwStreamerVideoBufferView& destination) {
  const auto& source_buffer = source.buffer;
  if (destination.memory_type != kMwStreamerMemoryHost &&
      destination.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("CudaFrame收到未知目标内存类型");
  }
  if (destination.storage_type != kMwStreamerVideoStorageLinear ||
      destination.pixel_format != source_buffer.pixel_format ||
      destination.width != source_buffer.width ||
      destination.height != source_buffer.height) {
    throw std::invalid_argument("CudaFrame目标视频格式与源帧不匹配");
  }

  const auto& source_linear = source_buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (!destination_linear.planes ||
      destination_linear.plane_count != source_linear.plane_count) {
    throw std::invalid_argument("CudaFrame目标视频平面数量不匹配");
  }
  for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
    const auto& source_plane = source_linear.planes[index];
    const auto& destination_plane = destination_linear.planes[index];
    const auto stride =
        static_cast<std::int64_t>(destination_plane.stride_bytes);
    const auto absolute_stride = stride < 0 ? -stride : stride;
    if (destination_plane.address == 0 || destination_plane.stride_bytes == 0 ||
        destination_plane.row_bytes != source_plane.row_bytes ||
        destination_plane.row_count != source_plane.row_count ||
        absolute_stride < destination_plane.row_bytes ||
        (destination.memory_type == kMwStreamerMemoryCuda && stride < 0)) {
      throw std::invalid_argument("CudaFrame目标视频平面布局不匹配");
    }
  }
}

}  // namespace

class CudaFrame::Impl final {
 public:
  explicit Impl(const MwStreamerVideoFrameView& source) : view_(source) {
    const auto& source_linear = source.buffer.storage.linear;
    plane_count_ = source_linear.plane_count;
    try {
      for (std::uint32_t index = 0; index < plane_count_; ++index) {
        const auto& source_plane = source_linear.planes[index];
        void* allocation = nullptr;
        std::size_t pitch = 0;
        ThrowIfCudaRuntimeError(
            cudaMallocPitch(&allocation, &pitch, source_plane.row_bytes,
                            source_plane.row_count),
            "分配CUDA视频平面");
        allocations_[index] = allocation;

        if (index == 0) {
          ThrowIfCudaError(cuCtxGetCurrent(&context_), "查询分配CUDA context");
          if (!context_) {
            throw std::runtime_error("CUDA视频平面分配后没有有效context");
          }
        }
        if (pitch > static_cast<std::size_t>(
                        std::numeric_limits<std::int32_t>::max())) {
          throw std::overflow_error("CudaFrame视频平面stride溢出");
        }
        planes_[index] = {
            reinterpret_cast<std::uintptr_t>(allocation),
            static_cast<std::int32_t>(pitch),
            source_plane.row_bytes,
            source_plane.row_count,
        };
      }
    } catch (...) {
      Release();
      throw;
    }

    view_.buffer.memory_type = kMwStreamerMemoryCuda;
    view_.buffer.storage_type = kMwStreamerVideoStorageLinear;
    view_.buffer.storage.linear = {planes_.data(), plane_count_};
  }

  ~Impl() { Release(); }

  void CopyHost(const MwStreamerVideoFrameView& source) {
    ScopedCudaContext context(context_);
    const auto& source_linear = source.buffer.storage.linear;
    for (std::uint32_t plane_index = 0; plane_index < plane_count_;
         ++plane_index) {
      const auto& source_plane = source_linear.planes[plane_index];
      const auto* source_base =
          reinterpret_cast<const std::uint8_t*>(source_plane.address);
      const auto destination =
          static_cast<CUdeviceptr>(planes_[plane_index].address);
      for (std::uint32_t row = 0; row < source_plane.row_count; ++row) {
        const auto* source_row =
            source_base +
            static_cast<std::ptrdiff_t>(row) * source_plane.stride_bytes;
        ThrowIfCudaError(
            cuMemcpyHtoD(destination + static_cast<std::size_t>(row) *
                                           planes_[plane_index].stride_bytes,
                         source_row, source_plane.row_bytes),
            "上传Host视频平面");
      }
    }
  }

  void CopyCuda(const MwStreamerVideoFrameView& source) {
    const auto& source_linear = source.buffer.storage.linear;
    for (std::uint32_t plane_index = 0; plane_index < plane_count_;
         ++plane_index) {
      const auto& source_plane = source_linear.planes[plane_index];
      CUDA_MEMCPY3D_PEER copy{};
      copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.srcDevice = static_cast<CUdeviceptr>(source_plane.address);
      copy.srcContext = GetPointerContext(source_plane.address);
      copy.srcPitch = static_cast<std::size_t>(source_plane.stride_bytes);
      copy.srcHeight = source_plane.row_count;
      copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.dstDevice = static_cast<CUdeviceptr>(planes_[plane_index].address);
      copy.dstContext = context_;
      copy.dstPitch =
          static_cast<std::size_t>(planes_[plane_index].stride_bytes);
      copy.dstHeight = planes_[plane_index].row_count;
      copy.WidthInBytes = source_plane.row_bytes;
      copy.Height = source_plane.row_count;
      copy.Depth = 1;
      ThrowIfCudaError(cuMemcpy3DPeer(&copy), "复制CUDA视频平面");
    }
  }

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }

  MwStreamerVideoFrameView& mutable_view() noexcept { return view_; }

  void CopyTo(const MwStreamerVideoBufferView& destination) const {
    ValidateDestination(view_, destination);
    const auto& source_linear = view_.buffer.storage.linear;
    const auto& destination_linear = destination.storage.linear;
    if (destination.memory_type == kMwStreamerMemoryHost) {
      ScopedCudaContext context(context_);
      for (std::uint32_t plane_index = 0; plane_index < plane_count_;
           ++plane_index) {
        const auto& source_plane = source_linear.planes[plane_index];
        const auto& destination_plane = destination_linear.planes[plane_index];
        auto* destination_base =
            reinterpret_cast<std::uint8_t*>(destination_plane.address);
        const auto source_base = static_cast<CUdeviceptr>(source_plane.address);
        for (std::uint32_t row = 0; row < source_plane.row_count; ++row) {
          ThrowIfCudaError(
              cuMemcpyDtoH(
                  destination_base + static_cast<std::ptrdiff_t>(row) *
                                         destination_plane.stride_bytes,
                  source_base +
                      static_cast<std::size_t>(row) * source_plane.stride_bytes,
                  source_plane.row_bytes),
              "复制CudaFrame到Host输出");
        }
      }
      return;
    }

    for (std::uint32_t plane_index = 0; plane_index < plane_count_;
         ++plane_index) {
      const auto& source_plane = source_linear.planes[plane_index];
      const auto& destination_plane = destination_linear.planes[plane_index];
      CUDA_MEMCPY3D_PEER copy{};
      copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.srcDevice = static_cast<CUdeviceptr>(source_plane.address);
      copy.srcContext = context_;
      copy.srcPitch = static_cast<std::size_t>(source_plane.stride_bytes);
      copy.srcHeight = source_plane.row_count;
      copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
      copy.dstDevice = static_cast<CUdeviceptr>(destination_plane.address);
      copy.dstContext = GetPointerContext(destination_plane.address);
      copy.dstPitch = static_cast<std::size_t>(destination_plane.stride_bytes);
      copy.dstHeight = destination_plane.row_count;
      copy.WidthInBytes = source_plane.row_bytes;
      copy.Height = source_plane.row_count;
      copy.Depth = 1;
      ThrowIfCudaError(cuMemcpy3DPeer(&copy), "复制CudaFrame到CUDA输出");
    }
  }

 private:
  void Release() noexcept {
    if (!context_) {
      return;
    }
    if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) {
      return;
    }
    for (auto& allocation : allocations_) {
      if (allocation) {
        cuMemFree(reinterpret_cast<CUdeviceptr>(allocation));
        allocation = nullptr;
      }
    }
    CUcontext popped_context = nullptr;
    cuCtxPopCurrent(&popped_context);
  }

  std::array<void*, kMaxPlaneCount> allocations_{};
  std::array<MwStreamerVideoPlaneView, kMaxPlaneCount> planes_{};
  std::uint32_t plane_count_ = 0;
  CUcontext context_ = nullptr;
  MwStreamerVideoFrameView view_{};
};

CudaFrame CudaFrame::CopyFrom(const MwStreamerVideoFrameView& source) {
  ValidateSource(source);
  EnsureCudaDriverInitialized();
  auto impl = std::make_unique<Impl>(source);
  if (source.buffer.memory_type == kMwStreamerMemoryHost) {
    impl->CopyHost(source);
  } else {
    impl->CopyCuda(source);
  }
  return CudaFrame(std::move(impl));
}

CudaFrame CudaFrame::AllocateLike(const MwStreamerVideoFrameView& prototype) {
  ValidateSource(prototype);
  EnsureCudaDriverInitialized();
  return CudaFrame(std::make_unique<Impl>(prototype));
}

CudaFrame::CudaFrame(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

CudaFrame::~CudaFrame() = default;

CudaFrame::CudaFrame(CudaFrame&& other) noexcept = default;

CudaFrame& CudaFrame::operator=(CudaFrame&& other) noexcept = default;

HostFrame CudaFrame::ToHost() const { return HostFrame::CopyFrom(view()); }

void CudaFrame::CopyTo(const MwStreamerVideoBufferView& destination) const {
  impl_->CopyTo(destination);
}

const MwStreamerVideoFrameView& CudaFrame::view() const noexcept {
  return impl_->view();
}

MwStreamerVideoFrameView& CudaFrame::mutable_view() noexcept {
  return impl_->mutable_view();
}

}  // namespace mw::streamer::opencv_adapter

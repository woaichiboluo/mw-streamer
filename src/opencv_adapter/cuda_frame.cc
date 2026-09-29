#include "mw/opencv_adapter/cuda_frame.h"

#include <cuda.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "mw/opencv_adapter/internal/cuda_driver.h"

namespace mw::opencv_adapter {
namespace {

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

class ScopedCudaContext final {
 public:
  explicit ScopedCudaContext(CUcontext context) {
    if (!context) {
      throw std::invalid_argument("CudaFrame要求有效的CUDA context");
    }
    CUcontext current = nullptr;
    ThrowIfCudaError(cuCtxGetCurrent(&current), "查询当前CUDA context");
    if (current == context) {
      return;
    }
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

void ValidateFrame(const MwStreamerVideoFrameView& frame) {
  const auto& buffer = frame.buffer;
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("CudaFrame只支持linear视频存储");
  }
  if (buffer.memory_type != kMwStreamerMemoryHost &&
      buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("CudaFrame收到未知视频内存类型");
  }
  if (buffer.pixel_format == kMwStreamerVideoPixelFormatUnknown ||
      buffer.width == 0 || buffer.height == 0) {
    throw std::invalid_argument("CudaFrame要求有效的视频格式和宽高");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes || linear.plane_count == 0 ||
      linear.plane_count > kMaxPlaneCount) {
    throw std::invalid_argument("CudaFrame视频平面数量无效");
  }
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    if (plane.address == 0 || plane.stride_bytes <= 0 || plane.row_bytes == 0 ||
        plane.row_count == 0 ||
        static_cast<std::uint32_t>(plane.stride_bytes) < plane.row_bytes) {
      throw std::invalid_argument(
          "CudaFrame快速路径要求有效的正stride视频平面: plane=" +
          std::to_string(index));
    }
  }
}

void ValidateDestination(const MwStreamerVideoFrameView& source,
                         const MwStreamerVideoBufferView& destination) {
  MwStreamerVideoFrameView destination_frame{};
  destination_frame.buffer = destination;
  ValidateFrame(destination_frame);
  if (destination.pixel_format != source.buffer.pixel_format ||
      destination.width != source.buffer.width ||
      destination.height != source.buffer.height) {
    throw std::invalid_argument("CudaFrame目标视频格式与源帧不匹配");
  }

  const auto& source_linear = source.buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (destination_linear.plane_count != source_linear.plane_count) {
    throw std::invalid_argument("CudaFrame目标视频平面数量不匹配");
  }
  for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
    const auto& source_plane = source_linear.planes[index];
    const auto& destination_plane = destination_linear.planes[index];
    if (destination_plane.row_bytes != source_plane.row_bytes ||
        destination_plane.row_count != source_plane.row_count) {
      throw std::invalid_argument("CudaFrame目标视频平面布局不匹配");
    }
  }
}

CUcontext GetPointerContext(std::uintptr_t address) {
  CUcontext context = nullptr;
  ThrowIfCudaError(cuPointerGetAttribute(&context, CU_POINTER_ATTRIBUTE_CONTEXT,
                                         static_cast<CUdeviceptr>(address)),
                   "查询CUDA视频平面context");
  if (!context) {
    throw std::invalid_argument("CUDA视频平面没有有效context");
  }
  return context;
}

void ValidateCudaPlanes(const MwStreamerVideoBufferView& buffer,
                        CUcontext context) {
  if (buffer.memory_type != kMwStreamerMemoryCuda) {
    return;
  }
  const auto& linear = buffer.storage.linear;
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    if (GetPointerContext(linear.planes[index].address) != context) {
      throw std::invalid_argument("CudaFrame不允许跨CUDA context复制视频平面");
    }
  }
}

void ValidateStream(CUcontext context, CUstream stream) {
  if (!stream) {
    return;
  }
  CUcontext stream_context = nullptr;
  ThrowIfCudaError(cuStreamGetCtx(stream, &stream_context),
                   "查询CUDA stream context");
  if (stream_context != context) {
    throw std::invalid_argument("CUDA stream不属于指定context");
  }
}

bool AreAllPlanesMergeable(
    const MwStreamerLinearVideoStorageView& source,
    const MwStreamerLinearVideoStorageView& destination) {
  if (source.plane_count < 2) {
    return false;
  }
  const auto source_pitch = source.planes[0].stride_bytes;
  const auto destination_pitch = destination.planes[0].stride_bytes;
  const auto row_bytes = source.planes[0].row_bytes;
  for (std::uint32_t index = 0; index < source.plane_count; ++index) {
    const auto& source_plane = source.planes[index];
    const auto& destination_plane = destination.planes[index];
    if (source_plane.stride_bytes != source_pitch ||
        destination_plane.stride_bytes != destination_pitch ||
        source_plane.row_bytes != row_bytes ||
        destination_plane.row_bytes != row_bytes) {
      return false;
    }
    if (index == 0) {
      continue;
    }
    const auto& previous_source = source.planes[index - 1];
    const auto& previous_destination = destination.planes[index - 1];
    const std::uint64_t source_offset =
        static_cast<std::uint64_t>(previous_source.stride_bytes) *
        previous_source.row_count;
    const std::uint64_t destination_offset =
        static_cast<std::uint64_t>(previous_destination.stride_bytes) *
        previous_destination.row_count;
    if (source_plane.address != previous_source.address + source_offset ||
        destination_plane.address !=
            previous_destination.address + destination_offset) {
      return false;
    }
  }
  return true;
}

CUDA_MEMCPY2D MakeCopy(const MwStreamerVideoPlaneView& source,
                       MwStreamerMemoryType source_type,
                       const MwStreamerVideoPlaneView& destination,
                       MwStreamerMemoryType destination_type,
                       std::size_t height) {
  CUDA_MEMCPY2D copy{};
  copy.srcPitch = static_cast<std::size_t>(source.stride_bytes);
  copy.dstPitch = static_cast<std::size_t>(destination.stride_bytes);
  if (source_type == kMwStreamerMemoryHost) {
    copy.srcMemoryType = CU_MEMORYTYPE_HOST;
    copy.srcHost = reinterpret_cast<const void*>(source.address);
  } else {
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = static_cast<CUdeviceptr>(source.address);
  }
  if (destination_type == kMwStreamerMemoryHost) {
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = reinterpret_cast<void*>(destination.address);
  } else {
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = static_cast<CUdeviceptr>(destination.address);
  }
  copy.WidthInBytes = source.row_bytes;
  copy.Height = height;
  return copy;
}

void EnqueueCopies(const MwStreamerVideoFrameView& source,
                   const MwStreamerVideoBufferView& destination,
                   CUstream stream) {
  const auto& source_linear = source.buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (AreAllPlanesMergeable(source_linear, destination_linear)) {
    std::size_t total_rows = 0;
    for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
      total_rows += source_linear.planes[index].row_count;
    }
    const auto copy = MakeCopy(
        source_linear.planes[0], source.buffer.memory_type,
        destination_linear.planes[0], destination.memory_type, total_rows);
    ThrowIfCudaError(cuMemcpy2DAsync(&copy, stream), "异步复制连续视频平面");
    return;
  }

  for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
    const auto copy =
        MakeCopy(source_linear.planes[index], source.buffer.memory_type,
                 destination_linear.planes[index], destination.memory_type,
                 source_linear.planes[index].row_count);
    ThrowIfCudaError(cuMemcpy2DAsync(&copy, stream), "异步复制视频平面");
  }
}

}  // namespace

class CudaFrame::Impl final {
 public:
  Impl(const MwStreamerVideoFrameView& prototype, CUcontext context)
      : context_(context), view_(prototype) {
    const auto& source_linear = prototype.buffer.storage.linear;
    plane_count_ = source_linear.plane_count;
    std::size_t total_rows = 0;
    std::size_t maximum_row_bytes = 0;
    for (std::uint32_t index = 0; index < plane_count_; ++index) {
      total_rows += source_linear.planes[index].row_count;
      maximum_row_bytes = std::max(
          maximum_row_bytes,
          static_cast<std::size_t>(source_linear.planes[index].row_bytes));
    }
    if (total_rows > std::numeric_limits<unsigned int>::max()) {
      throw std::overflow_error("CudaFrame视频总行数溢出");
    }

    ScopedCudaContext scoped_context(context_);
    std::size_t pitch = 0;
    ThrowIfCudaError(cuMemAllocPitch(&allocation_, &pitch, maximum_row_bytes,
                                     total_rows, 16),
                     "分配CUDA视频帧");
    if (pitch >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      Release();
      throw std::overflow_error("CudaFrame视频平面stride溢出");
    }

    std::size_t row_offset = 0;
    for (std::uint32_t index = 0; index < plane_count_; ++index) {
      const auto& source_plane = source_linear.planes[index];
      planes_[index] = {
          static_cast<std::uintptr_t>(allocation_ + pitch * row_offset),
          static_cast<std::int32_t>(pitch), source_plane.row_bytes,
          source_plane.row_count};
      row_offset += source_plane.row_count;
    }
    view_.buffer.memory_type = kMwStreamerMemoryCuda;
    view_.buffer.storage_type = kMwStreamerVideoStorageLinear;
    view_.buffer.storage.linear = {planes_.data(), plane_count_};
  }

  ~Impl() { Release(); }

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }
  CUcontext context() const noexcept { return context_; }

 private:
  void Release() noexcept {
    if (!allocation_ || !context_) {
      return;
    }
    if (cuCtxPushCurrent(context_) != CUDA_SUCCESS) {
      return;
    }
    cuMemFree(allocation_);
    allocation_ = 0;
    CUcontext popped_context = nullptr;
    cuCtxPopCurrent(&popped_context);
  }

  CUdeviceptr allocation_ = 0;
  std::array<MwStreamerVideoPlaneView, kMaxPlaneCount> planes_{};
  std::uint32_t plane_count_ = 0;
  CUcontext context_ = nullptr;
  MwStreamerVideoFrameView view_{};
};

CudaFrame CudaFrame::Allocate(const MwStreamerVideoFrameView& prototype,
                              CUcontext context) {
  ValidateFrame(prototype);
  EnsureCudaDriverInitialized();
  return CudaFrame(std::make_unique<Impl>(prototype, context));
}

void CudaFrame::Copy(const MwStreamerVideoFrameView& source,
                     const MwStreamerVideoBufferView& destination,
                     CUcontext context, CUstream stream) {
  ValidateFrame(source);
  ValidateDestination(source, destination);
  if (source.buffer.memory_type == kMwStreamerMemoryHost &&
      destination.memory_type == kMwStreamerMemoryHost) {
    throw std::invalid_argument("CudaFrame异步拷贝至少需要一个CUDA端点");
  }
  EnsureCudaDriverInitialized();
  ScopedCudaContext scoped_context(context);
  ValidateStream(context, stream);
  ValidateCudaPlanes(source.buffer, context);
  ValidateCudaPlanes(destination, context);
  EnqueueCopies(source, destination, stream);
}

CudaFrame::CudaFrame(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

CudaFrame::~CudaFrame() = default;

CudaFrame::CudaFrame(CudaFrame&& other) noexcept = default;

CudaFrame& CudaFrame::operator=(CudaFrame&& other) noexcept = default;

void CudaFrame::CopyFrom(const MwStreamerVideoFrameView& source,
                         CUstream stream) {
  Copy(source, view().buffer, context(), stream);
}

void CudaFrame::CopyTo(const MwStreamerVideoBufferView& destination,
                       CUstream stream) const {
  Copy(view(), destination, context(), stream);
}

const MwStreamerVideoFrameView& CudaFrame::view() const noexcept {
  return impl_->view();
}

CUcontext CudaFrame::context() const noexcept { return impl_->context(); }

}  // namespace mw::opencv_adapter

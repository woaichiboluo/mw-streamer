#include "mw/opencv_adapter/host_frame.h"

#include <cuda.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

void ValidateSource(const MwStreamerVideoFrameView& source) {
  const auto& buffer = source.buffer;
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("HostFrame只支持linear视频存储");
  }
  if (buffer.memory_type != kMwStreamerMemoryHost &&
      buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("HostFrame收到未知视频内存类型");
  }
  if (buffer.pixel_format == kMwStreamerVideoPixelFormatUnknown) {
    throw std::invalid_argument("HostFrame不能复制未知视频像素格式");
  }
  if (buffer.width == 0 || buffer.height == 0) {
    throw std::invalid_argument("HostFrame源视频缺少有效宽高");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes || linear.plane_count == 0 ||
      linear.plane_count > kMaxPlaneCount) {
    throw std::invalid_argument("HostFrame源视频平面数量无效");
  }
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    const auto stride = static_cast<std::int64_t>(plane.stride_bytes);
    const auto absolute_stride = stride < 0 ? -stride : stride;
    if (plane.address == 0 || plane.stride_bytes == 0 || plane.row_bytes == 0 ||
        plane.row_count == 0 || absolute_stride < plane.row_bytes) {
      throw std::invalid_argument("HostFrame源视频平面无效: plane=" +
                                  std::to_string(index));
    }
    if (buffer.memory_type == kMwStreamerMemoryCuda && stride < 0) {
      throw std::invalid_argument("HostFrame不支持负stride的CUDA视频平面");
    }
    if (plane.row_bytes >
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::overflow_error("HostFrame视频平面行宽溢出");
    }
    if (plane.row_bytes >
        std::numeric_limits<std::size_t>::max() / plane.row_count) {
      throw std::overflow_error("HostFrame视频平面大小溢出");
    }
  }
}

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
    throw std::invalid_argument("HostFrame收到未知目标内存类型");
  }
  if (destination.storage_type != kMwStreamerVideoStorageLinear ||
      destination.pixel_format != source_buffer.pixel_format ||
      destination.width != source_buffer.width ||
      destination.height != source_buffer.height) {
    throw std::invalid_argument("HostFrame目标视频格式与源帧不匹配");
  }

  const auto& source_linear = source_buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (!destination_linear.planes ||
      destination_linear.plane_count != source_linear.plane_count) {
    throw std::invalid_argument("HostFrame目标视频平面数量不匹配");
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
      throw std::invalid_argument("HostFrame目标视频平面布局不匹配");
    }
  }
}

}  // namespace

class HostFrame::Impl final {
 public:
  explicit Impl(const MwStreamerVideoFrameView& source) : view_(source) {
    const auto& source_linear = source.buffer.storage.linear;
    plane_count_ = source_linear.plane_count;
    for (std::uint32_t index = 0; index < plane_count_; ++index) {
      const auto& source_plane = source_linear.planes[index];
      storage_[index].resize(static_cast<std::size_t>(source_plane.row_bytes) *
                             source_plane.row_count);
      planes_[index] = {
          reinterpret_cast<std::uintptr_t>(storage_[index].data()),
          static_cast<std::int32_t>(source_plane.row_bytes),
          source_plane.row_bytes,
          source_plane.row_count,
      };
    }

    view_.buffer.memory_type = kMwStreamerMemoryHost;
    view_.buffer.storage_type = kMwStreamerVideoStorageLinear;
    view_.buffer.storage.linear = {planes_.data(), plane_count_};
  }

  void CopyHost(const MwStreamerVideoFrameView& source) {
    const auto& source_linear = source.buffer.storage.linear;
    for (std::uint32_t plane_index = 0; plane_index < plane_count_;
         ++plane_index) {
      const auto& source_plane = source_linear.planes[plane_index];
      auto* destination = storage_[plane_index].data();
      const auto* source_base =
          reinterpret_cast<const std::uint8_t*>(source_plane.address);
      for (std::uint32_t row = 0; row < source_plane.row_count; ++row) {
        const auto* source_row =
            source_base +
            static_cast<std::ptrdiff_t>(row) * source_plane.stride_bytes;
        std::memcpy(destination +
                        static_cast<std::size_t>(row) * source_plane.row_bytes,
                    source_row, source_plane.row_bytes);
      }
    }
  }

  void CopyCuda(const MwStreamerVideoFrameView& source) {
    EnsureCudaDriverInitialized();
    CUcontext source_context = nullptr;
    const auto first_address = static_cast<CUdeviceptr>(
        source.buffer.storage.linear.planes[0].address);
    ThrowIfCudaError(
        cuPointerGetAttribute(&source_context, CU_POINTER_ATTRIBUTE_CONTEXT,
                              first_address),
        "查询CUDA视频帧context");
    if (!source_context) {
      throw std::runtime_error("CUDA视频帧没有有效context");
    }

    ThrowIfCudaError(cuCtxPushCurrent(source_context), "设置CUDA视频帧context");
    try {
      const auto& source_linear = source.buffer.storage.linear;
      for (std::uint32_t plane_index = 0; plane_index < plane_count_;
           ++plane_index) {
        const auto& source_plane = source_linear.planes[plane_index];
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = static_cast<CUdeviceptr>(source_plane.address);
        copy.srcPitch = static_cast<std::size_t>(source_plane.stride_bytes);
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = storage_[plane_index].data();
        copy.dstPitch = source_plane.row_bytes;
        copy.WidthInBytes = source_plane.row_bytes;
        copy.Height = source_plane.row_count;
        ThrowIfCudaError(cuMemcpy2D(&copy), "下载CUDA视频平面");
      }
    } catch (...) {
      CUcontext popped_context = nullptr;
      cuCtxPopCurrent(&popped_context);
      throw;
    }

    CUcontext popped_context = nullptr;
    ThrowIfCudaError(cuCtxPopCurrent(&popped_context),
                     "恢复调用线程CUDA context");
  }

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }

  MwStreamerVideoFrameView& mutable_view() noexcept { return view_; }

  void CopyTo(const MwStreamerVideoBufferView& destination) const {
    ValidateDestination(view_, destination);
    const auto& source_linear = view_.buffer.storage.linear;
    const auto& destination_linear = destination.storage.linear;
    if (destination.memory_type == kMwStreamerMemoryHost) {
      for (std::uint32_t plane_index = 0; plane_index < plane_count_;
           ++plane_index) {
        const auto& source_plane = source_linear.planes[plane_index];
        const auto& destination_plane = destination_linear.planes[plane_index];
        const auto* source_base =
            reinterpret_cast<const std::uint8_t*>(source_plane.address);
        auto* destination_base =
            reinterpret_cast<std::uint8_t*>(destination_plane.address);
        for (std::uint32_t row = 0; row < source_plane.row_count; ++row) {
          std::memcpy(destination_base + static_cast<std::ptrdiff_t>(row) *
                                             destination_plane.stride_bytes,
                      source_base + static_cast<std::size_t>(row) *
                                        source_plane.stride_bytes,
                      source_plane.row_bytes);
        }
      }
      return;
    }

    EnsureCudaDriverInitialized();
    const CUcontext destination_context =
        GetPointerContext(destination_linear.planes[0].address);
    ThrowIfCudaError(cuCtxPushCurrent(destination_context),
                     "设置目标CUDA context");
    try {
      for (std::uint32_t plane_index = 0; plane_index < plane_count_;
           ++plane_index) {
        const auto& source_plane = source_linear.planes[plane_index];
        const auto& destination_plane = destination_linear.planes[plane_index];
        if (GetPointerContext(destination_plane.address) !=
            destination_context) {
          throw std::invalid_argument(
              "HostFrame目标CUDA视频平面不在同一context");
        }
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_HOST;
        copy.srcHost = reinterpret_cast<const void*>(source_plane.address);
        copy.srcPitch = static_cast<std::size_t>(source_plane.stride_bytes);
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = static_cast<CUdeviceptr>(destination_plane.address);
        copy.dstPitch =
            static_cast<std::size_t>(destination_plane.stride_bytes);
        copy.WidthInBytes = source_plane.row_bytes;
        copy.Height = source_plane.row_count;
        ThrowIfCudaError(cuMemcpy2D(&copy), "复制HostFrame到CUDA输出");
      }
    } catch (...) {
      CUcontext popped_context = nullptr;
      cuCtxPopCurrent(&popped_context);
      throw;
    }
    CUcontext popped_context = nullptr;
    ThrowIfCudaError(cuCtxPopCurrent(&popped_context),
                     "恢复调用线程CUDA context");
  }

 private:
  std::array<std::vector<std::uint8_t>, kMaxPlaneCount> storage_;
  std::array<MwStreamerVideoPlaneView, kMaxPlaneCount> planes_{};
  std::uint32_t plane_count_ = 0;
  MwStreamerVideoFrameView view_{};
};

HostFrame HostFrame::CopyFrom(const MwStreamerVideoFrameView& source) {
  ValidateSource(source);
  auto impl = std::make_unique<Impl>(source);
  if (source.buffer.memory_type == kMwStreamerMemoryHost) {
    impl->CopyHost(source);
  } else {
    impl->CopyCuda(source);
  }
  return HostFrame(std::move(impl));
}

HostFrame HostFrame::AllocateLike(const MwStreamerVideoFrameView& prototype) {
  ValidateSource(prototype);
  return HostFrame(std::make_unique<Impl>(prototype));
}

HostFrame::HostFrame(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

HostFrame::~HostFrame() = default;

HostFrame::HostFrame(HostFrame&& other) noexcept = default;

HostFrame& HostFrame::operator=(HostFrame&& other) noexcept = default;

void HostFrame::CopyTo(const MwStreamerVideoBufferView& destination) const {
  impl_->CopyTo(destination);
}

const MwStreamerVideoFrameView& HostFrame::view() const noexcept {
  return impl_->view();
}

MwStreamerVideoFrameView& HostFrame::mutable_view() noexcept {
  return impl_->mutable_view();
}

}  // namespace mw::streamer::opencv_adapter

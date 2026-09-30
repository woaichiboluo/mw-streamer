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

#include "mw/opencv_adapter/internal/cuda_driver.h"

namespace mw::opencv_adapter {
namespace {

constexpr std::size_t kMaxPlaneCount = 4;
constexpr std::size_t kAllocationPadding = 64;

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* error_name = nullptr;
  cuGetErrorName(result, &error_name);
  throw std::runtime_error(std::string(operation) + "失败: " +
                           (error_name ? error_name : "CUDA_ERROR_UNKNOWN"));
}

void ValidateHostFrame(const MwStreamerVideoFrameView& frame,
                       bool require_tight) {
  const auto& buffer = frame.buffer;
  if (buffer.memory_type != kMwStreamerMemoryHost) {
    throw std::invalid_argument("HostFrame只接受Host视频帧");
  }
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("HostFrame只支持linear视频存储");
  }
  if (buffer.pixel_format == kMwStreamerVideoPixelFormatUnknown ||
      buffer.width == 0 || buffer.height == 0) {
    throw std::invalid_argument("HostFrame要求有效的视频格式和宽高");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes || linear.plane_count == 0 ||
      linear.plane_count > kMaxPlaneCount) {
    throw std::invalid_argument("HostFrame视频平面数量无效");
  }
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    if (plane.address == 0 || plane.row_bytes == 0 || plane.row_count == 0 ||
        plane.stride_bytes <= 0 ||
        static_cast<std::uint32_t>(plane.stride_bytes) < plane.row_bytes ||
        (require_tight &&
         static_cast<std::uint32_t>(plane.stride_bytes) != plane.row_bytes) ||
        plane.row_bytes > static_cast<std::uint32_t>(
                              std::numeric_limits<std::int32_t>::max())) {
      throw std::invalid_argument(
          "HostFrame快速路径只接受紧密正stride视频平面: plane=" +
          std::to_string(index));
    }
    if (plane.row_bytes >
        std::numeric_limits<std::size_t>::max() / plane.row_count) {
      throw std::overflow_error("HostFrame视频平面大小溢出");
    }
  }
}

void ValidateDestination(const MwStreamerVideoFrameView& source,
                         const MwStreamerVideoBufferView& destination) {
  MwStreamerVideoFrameView destination_frame{};
  destination_frame.buffer = destination;
  ValidateHostFrame(destination_frame, true);
  if (destination.pixel_format != source.buffer.pixel_format ||
      destination.width != source.buffer.width ||
      destination.height != source.buffer.height) {
    throw std::invalid_argument("HostFrame目标视频格式与源帧不匹配");
  }

  const auto& source_linear = source.buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (destination_linear.plane_count != source_linear.plane_count) {
    throw std::invalid_argument("HostFrame目标视频平面数量不匹配");
  }
  for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
    if (destination_linear.planes[index].row_bytes !=
            source_linear.planes[index].row_bytes ||
        destination_linear.planes[index].row_count !=
            source_linear.planes[index].row_count) {
      throw std::invalid_argument("HostFrame目标视频平面布局不匹配");
    }
  }
}

bool AreAllPlanesContiguous(const MwStreamerLinearVideoStorageView& linear) {
  for (std::uint32_t index = 1; index < linear.plane_count; ++index) {
    const auto& previous = linear.planes[index - 1];
    const std::uint64_t previous_size =
        static_cast<std::uint64_t>(previous.row_bytes) * previous.row_count;
    if (linear.planes[index].address != previous.address + previous_size) {
      return false;
    }
  }
  return true;
}

std::size_t PayloadBytes(const MwStreamerLinearVideoStorageView& linear) {
  std::size_t result = 0;
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    const std::size_t plane_size =
        static_cast<std::size_t>(plane.row_bytes) * plane.row_count;
    if (result > std::numeric_limits<std::size_t>::max() - plane_size) {
      throw std::overflow_error("HostFrame视频总大小溢出");
    }
    result += plane_size;
  }
  return result;
}

void CopyHostPlanes(const MwStreamerVideoFrameView& source,
                    const MwStreamerVideoBufferView& destination) {
  const auto& source_linear = source.buffer.storage.linear;
  const auto& destination_linear = destination.storage.linear;
  if (AreAllPlanesContiguous(source_linear) &&
      AreAllPlanesContiguous(destination_linear)) {
    std::memcpy(reinterpret_cast<void*>(destination_linear.planes[0].address),
                reinterpret_cast<const void*>(source_linear.planes[0].address),
                PayloadBytes(source_linear));
    return;
  }

  for (std::uint32_t index = 0; index < source_linear.plane_count; ++index) {
    const auto& source_plane = source_linear.planes[index];
    const auto& destination_plane = destination_linear.planes[index];
    std::memcpy(reinterpret_cast<void*>(destination_plane.address),
                reinterpret_cast<const void*>(source_plane.address),
                static_cast<std::size_t>(source_plane.row_bytes) *
                    source_plane.row_count);
  }
}

}  // namespace

class HostFrame::Impl final {
 public:
  Impl(const MwStreamerVideoFrameView& prototype, CUcontext context)
      : context_(context), view_(prototype) {
    const auto& source_linear = prototype.buffer.storage.linear;
    plane_count_ = source_linear.plane_count;
    const std::size_t payload_bytes = PayloadBytes(source_linear);
    if (payload_bytes >
        std::numeric_limits<std::size_t>::max() - kAllocationPadding) {
      throw std::overflow_error("HostFrame视频分配大小溢出");
    }
    const std::size_t allocation_size = payload_bytes + kAllocationPadding;
    if (context_) {
      EnsureCudaDriverInitialized();
      ThrowIfCudaError(cuCtxPushCurrent(context_), "设置CUDA context");
      const CUresult allocation_result =
          cuMemHostAlloc(&allocation_, allocation_size, 0);
      CUcontext popped_context = nullptr;
      cuCtxPopCurrent(&popped_context);
      ThrowIfCudaError(allocation_result, "分配page-locked Host视频帧");
    } else {
      storage_.resize(allocation_size);
      allocation_ = storage_.data();
    }

    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < plane_count_; ++index) {
      const auto& source_plane = source_linear.planes[index];
      planes_[index] = {reinterpret_cast<std::uintptr_t>(allocation_) + offset,
                        static_cast<std::int32_t>(source_plane.row_bytes),
                        source_plane.row_bytes, source_plane.row_count};
      offset += static_cast<std::size_t>(source_plane.row_bytes) *
                source_plane.row_count;
    }

    view_.buffer.memory_type = kMwStreamerMemoryHost;
    view_.buffer.execution = {kMwStreamerExecutionCpu, nullptr, nullptr};
    view_.buffer.storage_type = kMwStreamerVideoStorageLinear;
    view_.buffer.storage.linear = {planes_.data(), plane_count_};
  }

  ~Impl() {
    if (!context_ || !allocation_) {
      return;
    }
    if (cuCtxPushCurrent(context_) == CUDA_SUCCESS) {
      cuMemFreeHost(allocation_);
      CUcontext popped_context = nullptr;
      cuCtxPopCurrent(&popped_context);
    }
  }

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }
  MwStreamerVideoFrameView& mutable_view() noexcept { return view_; }

 private:
  std::vector<std::uint8_t> storage_;
  void* allocation_ = nullptr;
  std::array<MwStreamerVideoPlaneView, kMaxPlaneCount> planes_{};
  std::uint32_t plane_count_ = 0;
  CUcontext context_ = nullptr;
  MwStreamerVideoFrameView view_{};
};

void HostFrame::Copy(const MwStreamerVideoFrameView& source,
                     const MwStreamerVideoBufferView& destination) {
  ValidateHostFrame(source, true);
  ValidateDestination(source, destination);
  CopyHostPlanes(source, destination);
}

HostFrame HostFrame::CopyFrom(const MwStreamerVideoFrameView& source) {
  ValidateHostFrame(source, true);
  auto impl = std::make_unique<Impl>(source, nullptr);
  CopyHostPlanes(source, impl->view().buffer);
  return HostFrame(std::move(impl));
}

HostFrame HostFrame::AllocatePinned(const MwStreamerVideoFrameView& prototype,
                                    CUcontext context) {
  ValidateHostFrame(prototype, false);
  if (!context) {
    throw std::invalid_argument("HostFrame要求有效的CUDA context");
  }
  return HostFrame(std::make_unique<Impl>(prototype, context));
}

HostFrame HostFrame::AllocateLike(const MwStreamerVideoFrameView& prototype) {
  ValidateHostFrame(prototype, false);
  return HostFrame(std::make_unique<Impl>(prototype, nullptr));
}

HostFrame::HostFrame(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

HostFrame::~HostFrame() = default;

HostFrame::HostFrame(HostFrame&& other) noexcept = default;

HostFrame& HostFrame::operator=(HostFrame&& other) noexcept = default;

void HostFrame::CopyTo(const MwStreamerVideoBufferView& destination) const {
  Copy(view(), destination);
}

const MwStreamerVideoFrameView& HostFrame::view() const noexcept {
  return impl_->view();
}

MwStreamerVideoFrameView& HostFrame::mutable_view() noexcept {
  return impl_->mutable_view();
}

}  // namespace mw::opencv_adapter

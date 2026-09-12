#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <future>
#include <vector>

#include "mw/opencv_adapter.h"

namespace {

using mw::opencv_adapter::CudaFrame;

TEST_CASE("CudaFrame支持多线程并发首次初始化CUDA Driver") {
  constexpr std::uint32_t kWidth = 64;
  constexpr std::uint32_t kHeight = 16;
  constexpr int kWorkerCount = 16;
  std::vector<std::uint8_t> y(kWidth * kHeight, 0x42);
  std::vector<std::uint8_t> uv(kWidth * kHeight / 2, 0x81);
  const std::array<MwStreamerVideoPlaneView, 2> planes = {{
      {reinterpret_cast<std::uintptr_t>(y.data()), kWidth, kWidth, kHeight},
      {reinterpret_cast<std::uintptr_t>(uv.data()), kWidth, kWidth,
       kHeight / 2},
  }};
  MwStreamerVideoFrameView source{};
  source.buffer.memory_type = kMwStreamerMemoryHost;
  source.buffer.storage_type = kMwStreamerVideoStorageLinear;
  source.buffer.pixel_format = kMwStreamerVideoPixelFormatNv12;
  source.buffer.width = kWidth;
  source.buffer.height = kHeight;
  source.buffer.storage.linear = {
      planes.data(), static_cast<std::uint32_t>(planes.size())};
  source.color = {kMwStreamerColorRangeLimited, kMwStreamerColorSpaceBt709,
                  kMwStreamerColorPrimariesBt709,
                  kMwStreamerColorTransferBt709,
                  kMwStreamerChromaLocationLeft};
  source.timestamp = {1, 1, {1, 25}};

  std::array<std::future<bool>, kWorkerCount> workers;
  for (auto& worker : workers) {
    worker = std::async(std::launch::async, [&source, kWidth, kHeight] {
      const auto cuda = CudaFrame::CopyFrom(source);
      const auto host = cuda.ToHost();
      const auto& linear = host.view().buffer.storage.linear;
      const auto* copied_y =
          reinterpret_cast<const std::uint8_t*>(linear.planes[0].address);
      const auto* copied_uv =
          reinterpret_cast<const std::uint8_t*>(linear.planes[1].address);
      return copied_y[0] == 0x42 && copied_y[kWidth * kHeight - 1] == 0x42 &&
             copied_uv[0] == 0x81 &&
             copied_uv[kWidth * (kHeight / 2) - 1] == 0x81;
    });
  }
  for (auto& worker : workers) {
    CHECK(worker.get());
  }
}

}  // namespace

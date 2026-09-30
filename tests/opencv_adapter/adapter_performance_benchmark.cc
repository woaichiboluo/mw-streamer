#include <cuda.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/cuda_mat_adapter.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/opencv_adapter/host_mat_adapter.h"

namespace {

using mw::opencv_adapter::CudaFrame;
using mw::opencv_adapter::CudaMatAdapter;
using mw::opencv_adapter::HostFrame;
using mw::opencv_adapter::HostMatAdapter;

constexpr std::uint32_t kWidth = 8160;
constexpr std::uint32_t kHeight = 2296;
constexpr int kWarmupIterations = 5;
constexpr int kCopyIterations = 100;
constexpr int kGpuColorIterations = 100;
constexpr int kCpuColorIterations = 10;

struct FormatSpec {
  MwStreamerVideoPixelFormat format;
  const char* name;
  std::uint32_t bytes_per_sample;
};

constexpr std::array kFormats = {
    FormatSpec{kMwStreamerVideoPixelFormatNv12, "NV12", 1},
    FormatSpec{kMwStreamerVideoPixelFormatP010, "P010", 2},
};

void ThrowIfCudaError(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) {
    return;
  }
  const char* error_name = nullptr;
  cuGetErrorName(result, &error_name);
  throw std::runtime_error(std::string(operation) + " failed: " +
                           (error_name ? error_name : "CUDA_ERROR_UNKNOWN"));
}

class CudaContext final {
 public:
  CudaContext() {
    ThrowIfCudaError(cuInit(0), "cuInit");
    ThrowIfCudaError(cuDeviceGet(&device_, 0), "cuDeviceGet");
    ThrowIfCudaError(cuCtxCreate(&context_, CU_CTX_SCHED_AUTO, device_),
                     "cuCtxCreate");
  }

  ~CudaContext() {
    if (context_) {
      cuCtxDestroy(context_);
    }
  }

  CudaContext(const CudaContext&) = delete;
  CudaContext& operator=(const CudaContext&) = delete;

  CUcontext get() const noexcept { return context_; }
  CUdevice device() const noexcept { return device_; }

 private:
  CUdevice device_ = 0;
  CUcontext context_ = nullptr;
};

class CudaStream final {
 public:
  CudaStream() {
    int least_priority = 0;
    int greatest_priority = 0;
    ThrowIfCudaError(
        cuCtxGetStreamPriorityRange(&least_priority, &greatest_priority),
        "cuCtxGetStreamPriorityRange");
    ThrowIfCudaError(cuStreamCreateWithPriority(
                         &stream_, CU_STREAM_NON_BLOCKING, greatest_priority),
                     "cuStreamCreateWithPriority");
  }

  ~CudaStream() {
    if (stream_) {
      cuStreamDestroy(stream_);
    }
  }

  CudaStream(const CudaStream&) = delete;
  CudaStream& operator=(const CudaStream&) = delete;

  CUstream get() const noexcept { return stream_; }

 private:
  CUstream stream_ = nullptr;
};

class HostVideoFrame final {
 public:
  HostVideoFrame(const FormatSpec& format, bool pinned, std::uint8_t value)
      : format_(format), pinned_(pinned) {
    const std::uint32_t row_bytes = kWidth * format.bytes_per_sample;
    const std::size_t luma_bytes =
        static_cast<std::size_t>(row_bytes) * kHeight;
    const std::size_t chroma_bytes = luma_bytes / 2;
    payload_bytes_ = luma_bytes + chroma_bytes;
    const std::size_t allocation_bytes = payload_bytes_ + 64;
    if (pinned_) {
      ThrowIfCudaError(cuMemHostAlloc(&allocation_, allocation_bytes, 0),
                       "cuMemHostAlloc");
    } else {
      storage_.resize(allocation_bytes);
      allocation_ = storage_.data();
    }
    std::memset(allocation_, value, allocation_bytes);
    planes_[0] = {reinterpret_cast<std::uintptr_t>(allocation_),
                  static_cast<std::int32_t>(row_bytes), row_bytes, kHeight};
    planes_[1] = {reinterpret_cast<std::uintptr_t>(allocation_) + luma_bytes,
                  static_cast<std::int32_t>(row_bytes), row_bytes, kHeight / 2};
    view_ = {
        {kMwStreamerMemoryHost,
         {kMwStreamerExecutionCpu, nullptr, nullptr},
         kMwStreamerVideoStorageLinear,
         format.format,
         kWidth,
         kHeight,
         {{planes_.data(), static_cast<std::uint32_t>(planes_.size())}}},
        {kMwStreamerColorRangeLimited, kMwStreamerColorSpaceBt709,
         kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
         kMwStreamerChromaLocationLeft},
        {0, 1, {1, 60}},
    };
  }

  ~HostVideoFrame() {
    if (pinned_ && allocation_) {
      cuMemFreeHost(allocation_);
    }
  }

  HostVideoFrame(const HostVideoFrame&) = delete;
  HostVideoFrame& operator=(const HostVideoFrame&) = delete;

  const MwStreamerVideoFrameView& view() const noexcept { return view_; }
  const MwStreamerVideoBufferView& buffer() const noexcept {
    return view_.buffer;
  }
  std::size_t payload_bytes() const noexcept { return payload_bytes_; }

  void Verify(std::uint8_t expected) const {
    const auto* bytes = static_cast<const std::uint8_t*>(allocation_);
    if (bytes[0] != expected || bytes[payload_bytes_ - 1] != expected) {
      throw std::runtime_error("copy verification failed");
    }
  }

 private:
  FormatSpec format_;
  bool pinned_ = false;
  std::vector<std::uint8_t> storage_;
  void* allocation_ = nullptr;
  std::array<MwStreamerVideoPlaneView, 2> planes_{};
  std::size_t payload_bytes_ = 0;
  MwStreamerVideoFrameView view_{};
};

struct Statistics {
  double minimum_ms;
  double mean_ms;
  double p50_ms;
  double p95_ms;
  double p99_ms;
  double maximum_ms;
  double standard_deviation_ms;
};

double Percentile(const std::vector<double>& sorted, double percentile) {
  const auto rank = static_cast<std::size_t>(
      std::ceil(percentile * static_cast<double>(sorted.size())));
  return sorted[std::max<std::size_t>(1, rank) - 1];
}

Statistics CalculateStatistics(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  double total = 0.0;
  for (const double sample : samples) {
    total += sample;
  }
  const double mean = total / static_cast<double>(samples.size());
  double squared_difference = 0.0;
  for (const double sample : samples) {
    const double difference = sample - mean;
    squared_difference += difference * difference;
  }
  return {samples.front(),
          mean,
          Percentile(samples, 0.50),
          Percentile(samples, 0.95),
          Percentile(samples, 0.99),
          samples.back(),
          std::sqrt(squared_difference / static_cast<double>(samples.size()))};
}

template <typename Operation>
Statistics MeasureCpu(int iterations, Operation&& operation) {
  for (int index = 0; index < kWarmupIterations; ++index) {
    operation();
  }
  std::vector<double> samples;
  samples.reserve(iterations);
  for (int index = 0; index < iterations; ++index) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(
        std::chrono::duration<double, std::milli>(end - start).count());
  }
  return CalculateStatistics(std::move(samples));
}

template <typename Operation>
Statistics MeasureCuda(CUstream stream, int iterations, Operation&& operation) {
  CUevent start = nullptr;
  CUevent end = nullptr;
  ThrowIfCudaError(cuEventCreate(&start, CU_EVENT_DEFAULT),
                   "cuEventCreate(start)");
  try {
    ThrowIfCudaError(cuEventCreate(&end, CU_EVENT_DEFAULT),
                     "cuEventCreate(end)");
    for (int index = 0; index < kWarmupIterations; ++index) {
      operation();
    }
    ThrowIfCudaError(cuStreamSynchronize(stream), "warmup synchronization");

    std::vector<double> samples;
    samples.reserve(iterations);
    for (int index = 0; index < iterations; ++index) {
      ThrowIfCudaError(cuEventRecord(start, stream), "cuEventRecord(start)");
      operation();
      ThrowIfCudaError(cuEventRecord(end, stream), "cuEventRecord(end)");
      ThrowIfCudaError(cuEventSynchronize(end), "cuEventSynchronize");
      float elapsed_ms = 0.0F;
      ThrowIfCudaError(cuEventElapsedTime(&elapsed_ms, start, end),
                       "cuEventElapsedTime");
      samples.push_back(elapsed_ms);
    }
    cuEventDestroy(end);
    cuEventDestroy(start);
    return CalculateStatistics(std::move(samples));
  } catch (...) {
    if (end) {
      cuEventDestroy(end);
    }
    cuEventDestroy(start);
    throw;
  }
}

void PrintHeader(const char* title, const char* throughput_unit) {
  fmt::print("\n{}\n", title);
  fmt::print("{:<35} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>8} {:>9}\n",
             "operation", "min", "mean", "p50", "p95", "p99", "max", "fps",
             throughput_unit);
}

void PrintResult(const std::string& name, const Statistics& statistics,
                 double throughput) {
  fmt::print(
      "{:<35} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f} {:>8.3f} "
      "{:>8.1f} {:>9.2f}\n",
      name, statistics.minimum_ms, statistics.mean_ms, statistics.p50_ms,
      statistics.p95_ms, statistics.p99_ms, statistics.maximum_ms,
      1000.0 / statistics.p50_ms, throughput);
}

void PrintCopyResult(const std::string& name, const Statistics& statistics,
                     std::size_t payload_bytes) {
  PrintResult(
      name, statistics,
      static_cast<double>(payload_bytes) / statistics.p50_ms / 1'000'000.0);
}

void PrintColorResult(const std::string& name, const Statistics& statistics) {
  const double pixels = static_cast<double>(kWidth) * kHeight;
  PrintResult(name, statistics, pixels / statistics.p50_ms / 1000.0);
}

void RunCopyBenchmarks(CUcontext context, CUstream stream) {
  PrintHeader("copy benchmarks (preallocated fast paths)", "GB/s");
  for (const auto& format : kFormats) {
    HostVideoFrame pageable_source(format, false, 0x35);
    HostVideoFrame pageable_destination(format, false, 0x00);
    HostVideoFrame pinned_source(format, true, 0x35);
    HostVideoFrame pinned_destination(format, true, 0x00);
    auto cuda_source = CudaFrame::Allocate(pinned_source.view(), context);
    auto cuda_destination = CudaFrame::Allocate(pinned_source.view(), context);
    cuda_source.CopyFrom(pinned_source.view(), stream);
    ThrowIfCudaError(cuStreamSynchronize(stream), "initial upload");
    const std::string prefix = std::string(format.name) + " ";

    PrintCopyResult(prefix + "Host->Host contiguous",
                    MeasureCpu(kCopyIterations,
                               [&] {
                                 HostFrame::Copy(pageable_source.view(),
                                                 pageable_destination.buffer());
                               }),
                    pageable_source.payload_bytes());
    pageable_destination.Verify(0x35);

    PrintCopyResult(
        prefix + "Host->CUDA pageable",
        MeasureCpu(kCopyIterations,
                   [&] {
                     cuda_destination.CopyFrom(pageable_source.view(), stream);
                     ThrowIfCudaError(cuStreamSynchronize(stream),
                                      "pageable Host-to-CUDA synchronization");
                   }),
        pageable_source.payload_bytes());
    PrintCopyResult(
        prefix + "CUDA->Host pageable",
        MeasureCpu(kCopyIterations,
                   [&] {
                     cuda_source.CopyTo(pageable_destination.buffer(), stream);
                     ThrowIfCudaError(cuStreamSynchronize(stream),
                                      "pageable CUDA-to-Host synchronization");
                   }),
        pageable_source.payload_bytes());
    pageable_destination.Verify(0x35);

    PrintCopyResult(
        prefix + "Host->CUDA pinned",
        MeasureCuda(
            stream, kCopyIterations,
            [&] { cuda_destination.CopyFrom(pinned_source.view(), stream); }),
        pinned_source.payload_bytes());
    PrintCopyResult(
        prefix + "CUDA->Host pinned",
        MeasureCuda(
            stream, kCopyIterations,
            [&] { cuda_source.CopyTo(pinned_destination.buffer(), stream); }),
        pinned_source.payload_bytes());
    pinned_destination.Verify(0x35);
    PrintCopyResult(prefix + "CUDA->CUDA same context",
                    MeasureCuda(stream, kCopyIterations,
                                [&] {
                                  CudaFrame::Copy(
                                      cuda_source.view(),
                                      cuda_destination.view().buffer, stream);
                                }),
                    pinned_source.payload_bytes());
  }
}

void RunColorBenchmarks(CUcontext context, CUstream stream) {
  PrintHeader("color conversion benchmarks", "MPixel/s");
  for (const auto& format : kFormats) {
    HostVideoFrame pinned_yuv(format, true, 0x40);
    auto cuda_yuv = CudaFrame::Allocate(pinned_yuv.view(), context);
    auto cuda_output = CudaFrame::Allocate(pinned_yuv.view(), context);
    cuda_yuv.CopyFrom(pinned_yuv.view(), stream);
    ThrowIfCudaError(cuStreamSynchronize(stream), "color input upload");
    const int bgr_type = format.bytes_per_sample == 1 ? CV_8UC3 : CV_16UC3;
    cv::cuda::GpuMat cuda_bgr(static_cast<int>(kHeight),
                              static_cast<int>(kWidth), bgr_type);
    const std::string prefix = std::string(format.name) + " ";

    PrintColorResult(
        prefix + "CUDA YUV->BGR", MeasureCuda(stream, kGpuColorIterations, [&] {
          CudaMatAdapter::ToBgr(cuda_yuv.view(), &cuda_bgr, stream);
        }));
    PrintColorResult(
        prefix + "CUDA BGR->YUV", MeasureCuda(stream, kGpuColorIterations, [&] {
          CudaMatAdapter::FromBgr(cuda_bgr, pinned_yuv.view().color,
                                  cuda_output.view().buffer, stream);
        }));
    PrintColorResult(
        prefix + "CUDA round trip",
        MeasureCuda(stream, kGpuColorIterations, [&] {
          CudaMatAdapter::ToBgr(cuda_yuv.view(), &cuda_bgr, stream);
          CudaMatAdapter::FromBgr(cuda_bgr, pinned_yuv.view().color,
                                  cuda_output.view().buffer, stream);
        }));

    PrintColorResult(
        prefix + "CPU YUV->BGR", MeasureCpu(kCpuColorIterations, [&] {
          const auto result = HostMatAdapter::ToBgr(pinned_yuv.view());
          (void)result;
        }));
    const cv::Mat host_bgr = HostMatAdapter::ToBgr(pinned_yuv.view());
    PrintColorResult(prefix + "CPU BGR->YUV",
                     MeasureCpu(kCpuColorIterations, [&] {
                       const auto result =
                           HostMatAdapter::FromBgr(host_bgr, pinned_yuv.view());
                       (void)result;
                     }));
  }
}

void PrintEnvironment(CUdevice device) {
  std::array<char, 256> name{};
  ThrowIfCudaError(cuDeviceGetName(name.data(), name.size(), device),
                   "cuDeviceGetName");
  int driver_version = 0;
  ThrowIfCudaError(cuDriverGetVersion(&driver_version), "cuDriverGetVersion");
  fmt::print("mw OpenCV adapter benchmark\n");
  fmt::print("frame={}x{}, GPU={}, CUDA driver={}.{}, CPU threads={}\n", kWidth,
             kHeight, name.data(), driver_version / 1000,
             driver_version % 1000 / 10, std::thread::hardware_concurrency());
  fmt::print("warmup={}, copy samples={}, GPU color samples={}\n",
             kWarmupIterations, kCopyIterations, kGpuColorIterations);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const bool copy_only =
        argc == 2 && std::string_view(argv[1]) == "--copy-only";
    const bool color_only =
        argc == 2 && std::string_view(argv[1]) == "--color-only";
    if (argc > 2 || (argc == 2 && !copy_only && !color_only)) {
      throw std::invalid_argument(
          "usage: mw_opencv_adapter_benchmark [--copy-only|--color-only]");
    }

    CudaContext context;
    CudaStream stream;
    PrintEnvironment(context.device());
    if (!color_only) {
      RunCopyBenchmarks(context.get(), stream.get());
    }
    if (!copy_only) {
      RunColorBenchmarks(context.get(), stream.get());
    }
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "benchmark failed: {}\n", error.what());
    return 1;
  }
}

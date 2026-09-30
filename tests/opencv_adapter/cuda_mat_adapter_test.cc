#include "mw/opencv_adapter/cuda_mat_adapter.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>
#include <thread>
#include <vector>

#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/opencv_adapter/host_mat_adapter.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hardware_context.h"
#include "mw/streamer/processor/internal/frame_adapter.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace {

using mw::opencv_adapter::CudaFrame;
using mw::opencv_adapter::CudaMatAdapter;
using mw::opencv_adapter::HostFrame;
using mw::opencv_adapter::HostMatAdapter;
using mw::streamer::Frame;
using mw::streamer::HardwareContext;
using mw::streamer::ThrowIfError;
using mw::streamer::internal::VideoFrameAdapter;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 16;

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

HostFrame MakePinnedCopy(const MwStreamerVideoFrameView& source,
                         CUcontext context) {
  auto result = HostFrame::AllocatePinned(source, context);
  HostFrame::Copy(source, result.view().buffer);
  return result;
}

class TestFrame final {
 public:
  explicit TestFrame(MwStreamerVideoPixelFormat format,
                     std::uint32_t width = kWidth,
                     std::uint32_t height = kHeight)
      : width_(width), height_(height) {
    const bool is_16_bit = format == kMwStreamerVideoPixelFormatP010 ||
                           format == kMwStreamerVideoPixelFormatP016 ||
                           format == kMwStreamerVideoPixelFormatYuv444p16le;
    const bool is_planar = format == kMwStreamerVideoPixelFormatYuv444p ||
                           format == kMwStreamerVideoPixelFormatYuv444p16le;
    const std::uint32_t bytes_per_sample = is_16_bit ? 2 : 1;
    const std::uint32_t row_bytes = width_ * bytes_per_sample;
    const std::uint32_t plane_count = is_planar ? 3 : 2;
    storage_.resize(plane_count);
    planes_.resize(plane_count);
    for (std::uint32_t index = 0; index < plane_count; ++index) {
      const std::uint32_t rows =
          is_planar || index == 0 ? height_ : height_ / 2;
      storage_[index].resize(static_cast<std::size_t>(row_bytes) * rows);
      planes_[index] = {
          reinterpret_cast<std::uintptr_t>(storage_[index].data()),
          static_cast<std::int32_t>(row_bytes), row_bytes, rows};
    }

    if (is_16_bit) {
      const std::uint16_t black =
          format == kMwStreamerVideoPixelFormatP010 ? 64U << 6 : 16U << 8;
      const std::uint16_t white =
          format == kMwStreamerVideoPixelFormatP010 ? 940U << 6 : 235U << 8;
      FillLuma(black, white);
      FillChroma<std::uint16_t>(
          format == kMwStreamerVideoPixelFormatP010 ? 512U << 6 : 128U << 8);
    } else {
      FillLuma<std::uint8_t>(16, 235);
      FillChroma<std::uint8_t>(128);
    }

    view_ = {
        {kMwStreamerMemoryHost,
         {kMwStreamerExecutionCpu, nullptr, nullptr},
         kMwStreamerVideoStorageLinear,
         format,
         width_,
         height_,
         {{planes_.data(), plane_count}}},
        MakeColorInfo(),
        {1234, 1, {1, 25}},
    };
  }

  const MwStreamerVideoFrameView& view() const { return view_; }

 private:
  template <typename Sample>
  void FillLuma(Sample black, Sample white) {
    auto* data = reinterpret_cast<Sample*>(storage_[0].data());
    for (std::uint32_t row = 0; row < height_; ++row) {
      const Sample value = row < height_ / 2 ? black : white;
      std::fill_n(data + static_cast<std::size_t>(row) * width_, width_, value);
    }
  }

  template <typename Sample>
  void FillChroma(Sample neutral) {
    for (std::size_t index = 1; index < storage_.size(); ++index) {
      auto* data = reinterpret_cast<Sample*>(storage_[index].data());
      std::fill_n(data, storage_[index].size() / sizeof(Sample), neutral);
    }
  }

  std::vector<std::vector<std::uint8_t>> storage_;
  std::vector<MwStreamerVideoPlaneView> planes_;
  std::uint32_t width_;
  std::uint32_t height_;
  MwStreamerVideoFrameView view_{};
};

void CheckMatsNear(const cv::Mat& expected, const cv::Mat& actual,
                   int tolerance) {
  REQUIRE(actual.rows == expected.rows);
  REQUIRE(actual.cols == expected.cols);
  REQUIRE(actual.type() == expected.type());
  int maximum_error = 0;
  if (expected.depth() == CV_8U) {
    for (int row = 0; row < expected.rows; ++row) {
      const auto* expected_row = expected.ptr<std::uint8_t>(row);
      const auto* actual_row = actual.ptr<std::uint8_t>(row);
      for (int column = 0; column < expected.cols * 3; ++column) {
        maximum_error = std::max(
            maximum_error, std::abs(static_cast<int>(actual_row[column]) -
                                    static_cast<int>(expected_row[column])));
      }
    }
  } else {
    for (int row = 0; row < expected.rows; ++row) {
      const auto* expected_row = expected.ptr<std::uint16_t>(row);
      const auto* actual_row = actual.ptr<std::uint16_t>(row);
      for (int column = 0; column < expected.cols * 3; ++column) {
        maximum_error = std::max(
            maximum_error, std::abs(static_cast<int>(actual_row[column]) -
                                    static_cast<int>(expected_row[column])));
      }
    }
  }
  if (expected.depth() == CV_8U) {
    CAPTURE(expected.at<cv::Vec3b>(0, 0), actual.at<cv::Vec3b>(0, 0));
  } else {
    const auto expected_pixel = expected.at<cv::Vec<std::uint16_t, 3>>(0, 0);
    const auto actual_pixel = actual.at<cv::Vec<std::uint16_t, 3>>(0, 0);
    CAPTURE(expected_pixel, actual_pixel);
  }
  CHECK(maximum_error <= tolerance);
}

void CheckP010IsQuantized(const HostFrame& host) {
  const auto& linear = host.view().buffer.storage.linear;
  for (std::uint32_t plane_index = 0; plane_index < linear.plane_count;
       ++plane_index) {
    const auto& plane = linear.planes[plane_index];
    for (std::uint32_t row = 0; row < plane.row_count; ++row) {
      const auto* values = reinterpret_cast<const std::uint16_t*>(
          plane.address +
          static_cast<std::uintptr_t>(row) * plane.stride_bytes);
      for (std::uint32_t column = 0; column < plane.row_bytes / 2; ++column) {
        CHECK((values[column] & 0x3fU) == 0);
      }
    }
  }
}

TEST_CASE("CudaMatAdapter在同一context中转换YUV和BGR") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  constexpr std::array kFormats = {
      kMwStreamerVideoPixelFormatNv12,
      kMwStreamerVideoPixelFormatP010,
      kMwStreamerVideoPixelFormatP016,
  };

  for (const auto format : kFormats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      const TestFrame host_source(format);
      const cv::Mat host_expected = HostMatAdapter::ToBgr(host_source.view());

      CUcontext context = nullptr;
      REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
      REQUIRE(context != nullptr);
      const auto pinned_source = MakePinnedCopy(host_source.view(), context);
      auto pinned_output = MakePinnedCopy(host_source.view(), context);
      CUstream stream = nullptr;
      REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);

      auto cuda_source = CudaFrame::Allocate(host_source.view(), context);
      auto cuda_output = CudaFrame::Allocate(host_source.view(), context);
      const int bgr_type =
          format == kMwStreamerVideoPixelFormatNv12 ? CV_8UC3 : CV_16UC3;
      cv::cuda::GpuMat gpu_bgr(kHeight, kWidth, bgr_type);
      cv::cuda::GpuMat round_trip_gpu(kHeight, kWidth, bgr_type);

      cuda_source.CopyFrom(pinned_source.view(), stream);
      CudaMatAdapter::ToBgr(cuda_source.view(), &gpu_bgr, stream);
      CudaMatAdapter::FromBgr(gpu_bgr, host_source.view().color,
                              cuda_output.view().buffer, stream);
      CudaMatAdapter::ToBgr(cuda_output.view(), &round_trip_gpu, stream);
      cuda_output.CopyTo(pinned_output.view().buffer, stream);
      REQUIRE(cuStreamSynchronize(stream) == CUDA_SUCCESS);

      cv::Mat gpu_actual;
      gpu_bgr.download(gpu_actual);
      CheckMatsNear(host_expected, gpu_actual,
                    host_expected.depth() == CV_8U ? 3 : 768);

      CHECK(cuda_output.view().buffer.memory_type == kMwStreamerMemoryCuda);
      CHECK(cuda_output.view().buffer.pixel_format == format);
      CHECK(cuda_output.view().timestamp.pts == 1234);
      if (format == kMwStreamerVideoPixelFormatP010) {
        CheckP010IsQuantized(pinned_output);
      }

      cv::Mat round_trip;
      round_trip_gpu.download(round_trip);
      CheckMatsNear(gpu_actual, round_trip,
                    host_expected.depth() == CV_8U ? 4 : 1024);
      REQUIRE(cuStreamDestroy(stream) == CUDA_SUCCESS);
    }
  }
}

TEST_CASE("CudaMatAdapter异步直接转换不等待调用方stream") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const TestFrame host_source(kMwStreamerVideoPixelFormatNv12);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  REQUIRE(context != nullptr);
  const auto pinned_source = MakePinnedCopy(host_source.view(), context);
  auto cuda_source = CudaFrame::Allocate(host_source.view(), context);
  const auto& source_view = cuda_source.view();
  cv::cuda::GpuMat bgr(kHeight, kWidth, CV_8UC3);
  CUstream stream = nullptr;
  REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);

  cuda_source.CopyFrom(pinned_source.view(), stream);
  CudaMatAdapter::ToBgr(source_view, &bgr, stream);
  REQUIRE(cuStreamSynchronize(stream) == CUDA_SUCCESS);
  std::atomic<bool> preceding_work_finished = false;
  REQUIRE(cuLaunchHostFunc(
              stream,
              [](void* state) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                static_cast<std::atomic<bool>*>(state)->store(true);
              },
              &preceding_work_finished) == CUDA_SUCCESS);

  CudaMatAdapter::ToBgr(source_view, &bgr, stream);
  CHECK_FALSE(preceding_work_finished.load());
  REQUIRE(cuStreamSynchronize(stream) == CUDA_SUCCESS);
  CHECK(preceding_work_finished.load());
  REQUIRE(cuStreamDestroy(stream) == CUDA_SUCCESS);
}

TEST_CASE("CudaMatAdapter省略stream时使用default stream") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const TestFrame host_source(kMwStreamerVideoPixelFormatNv12);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  REQUIRE(context != nullptr);
  const auto pinned_source = MakePinnedCopy(host_source.view(), context);
  auto pinned_output = MakePinnedCopy(host_source.view(), context);
  auto cuda_source = CudaFrame::Allocate(host_source.view(), context);
  auto cuda_output = CudaFrame::Allocate(host_source.view(), context);
  cv::cuda::GpuMat bgr(kHeight, kWidth, CV_8UC3);

  cuda_source.CopyFrom(pinned_source.view());
  CudaMatAdapter::ToBgr(cuda_source.view(), &bgr);
  CudaMatAdapter::FromBgr(bgr, host_source.view().color,
                          cuda_output.view().buffer);
  cuda_output.CopyTo(pinned_output.view().buffer);
  REQUIRE(cuStreamSynchronize(nullptr) == CUDA_SUCCESS);

  CHECK(cuda_output.view().buffer.memory_type == kMwStreamerMemoryCuda);
  CHECK(cuda_output.view().timestamp.pts == 1234);
}

TEST_CASE("CudaMatAdapter异步直接转换拒绝隐式拷贝") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const TestFrame host_source(kMwStreamerVideoPixelFormatNv12);
  cv::cuda::GpuMat bgr(kHeight, kWidth, CV_8UC3);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  REQUIRE(context != nullptr);
  CUstream stream = nullptr;
  REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
  CHECK_THROWS_AS(CudaMatAdapter::ToBgr(host_source.view(), &bgr, stream),
                  std::invalid_argument);

  const TestFrame host_444(kMwStreamerVideoPixelFormatYuv444p);
  auto cuda_444 = CudaFrame::Allocate(host_444.view(), context);
  CHECK_THROWS_AS(CudaMatAdapter::ToBgr(cuda_444.view(), &bgr, stream),
                  std::invalid_argument);
  REQUIRE(cuStreamDestroy(stream) == CUDA_SUCCESS);
}

TEST_CASE("CudaMatAdapter拒绝HDR和不匹配的GpuMat") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const TestFrame host_source(kMwStreamerVideoPixelFormatNv12);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  REQUIRE(context != nullptr);
  auto cuda_source = CudaFrame::Allocate(host_source.view(), context);
  auto cuda_output = CudaFrame::Allocate(host_source.view(), context);
  CUstream stream = nullptr;
  REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);

  auto hdr = cuda_source.view();
  hdr.color.transfer = kMwStreamerColorTransferSmpte2084;
  cv::cuda::GpuMat valid_bgr(kHeight, kWidth, CV_8UC3);
  CHECK_THROWS_AS(CudaMatAdapter::ToBgr(hdr, &valid_bgr, stream),
                  std::invalid_argument);

  cv::cuda::GpuMat wrong_type(kHeight, kWidth, CV_16UC3);
  CHECK_THROWS_AS(CudaMatAdapter::FromBgr(wrong_type, host_source.view().color,
                                          cuda_output.view().buffer, stream),
                  std::invalid_argument);
  REQUIRE(cuStreamDestroy(stream) == CUDA_SUCCESS);
}

TEST_CASE("CudaMatAdapter使用调用方stream串联GpuMat生产") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const TestFrame prototype(kMwStreamerVideoPixelFormatNv12);
  CUcontext context = nullptr;
  REQUIRE(cuCtxGetCurrent(&context) == CUDA_SUCCESS);
  REQUIRE(context != nullptr);
  auto output = CudaFrame::Allocate(prototype.view(), context);
  cv::cuda::GpuMat source(kHeight, kWidth, CV_8UC3);
  cudaStream_t producer = nullptr;
  REQUIRE(cudaStreamCreateWithFlags(&producer, cudaStreamNonBlocking) ==
          cudaSuccess);

  std::atomic<bool> producer_finished = false;
  REQUIRE(cudaLaunchHostFunc(
              producer,
              [](void* state) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                static_cast<std::atomic<bool>*>(state)->store(true);
              },
              &producer_finished) == cudaSuccess);
  REQUIRE(cudaMemset2DAsync(source.data, source.step, 128,
                            static_cast<std::size_t>(kWidth) * 3, kHeight,
                            producer) == cudaSuccess);

  CudaMatAdapter::FromBgr(source, prototype.view().color, output.view().buffer,
                          reinterpret_cast<CUstream>(producer));
  CHECK_FALSE(producer_finished.load());
  REQUIRE(cudaStreamSynchronize(producer) == cudaSuccess);
  CHECK(producer_finished.load());
  CHECK(output.view().buffer.memory_type == kMwStreamerMemoryCuda);
  CHECK(cudaStreamDestroy(producer) == cudaSuccess);
}

TEST_CASE("CudaMatAdapter使用FFmpeg context并拒绝跨context转换") {
  constexpr int kContextWidth = 64;
  constexpr int kContextHeight = 64;
  const auto hardware_context = HardwareContext::CreateCuda(0);
  AVBufferRef* frames_ref =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(hardware_context.get()));
  REQUIRE(frames_ref != nullptr);
  auto* frames_context = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
  frames_context->format = AV_PIX_FMT_CUDA;
  frames_context->sw_format = AV_PIX_FMT_NV12;
  frames_context->width = kContextWidth;
  frames_context->height = kContextHeight;
  frames_context->initial_pool_size = 1;
  ThrowIfError(av_hwframe_ctx_init(frames_ref), "初始化测试CUDA帧池");

  Frame host;
  host->format = AV_PIX_FMT_NV12;
  host->width = kContextWidth;
  host->height = kContextHeight;
  ThrowIfError(av_frame_get_buffer(host.get(), 32), "分配测试Host帧");
  std::memset(host->data[0], 96,
              static_cast<std::size_t>(host->linesize[0]) * kContextHeight);
  std::memset(
      host->data[1], 128,
      static_cast<std::size_t>(host->linesize[1]) * (kContextHeight / 2));

  Frame ffmpeg_cuda;
  ThrowIfError(av_hwframe_get_buffer(frames_ref, ffmpeg_cuda.get(), 0),
               "分配测试FFmpeg CUDA帧");
  av_buffer_unref(&frames_ref);
  ThrowIfError(av_hwframe_transfer_data(ffmpeg_cuda.get(), host.get(), 0),
               "上传测试FFmpeg CUDA帧");
  ffmpeg_cuda->time_base = {1, 25};
  ffmpeg_cuda->pts = 1;
  ffmpeg_cuda->duration = 1;
  ffmpeg_cuda->color_range = AVCOL_RANGE_MPEG;
  ffmpeg_cuda->colorspace = AVCOL_SPC_BT709;
  ffmpeg_cuda->color_primaries = AVCOL_PRI_BT709;
  ffmpeg_cuda->color_trc = AVCOL_TRC_BT709;
  ffmpeg_cuda->chroma_location = AVCHROMA_LOC_LEFT;
  host->time_base = ffmpeg_cuda->time_base;
  host->pts = ffmpeg_cuda->pts;
  host->duration = ffmpeg_cuda->duration;
  host->color_range = ffmpeg_cuda->color_range;
  host->colorspace = ffmpeg_cuda->colorspace;
  host->color_primaries = ffmpeg_cuda->color_primaries;
  host->color_trc = ffmpeg_cuda->color_trc;
  host->chroma_location = ffmpeg_cuda->chroma_location;
  const VideoFrameAdapter adapter(ffmpeg_cuda);
  const VideoFrameAdapter host_adapter(host);

  CUcontext source_context = nullptr;
  REQUIRE(cuPointerGetAttribute(
              &source_context, CU_POINTER_ATTRIBUTE_CONTEXT,
              static_cast<CUdeviceptr>(
                  adapter.view().buffer.storage.linear.planes[0].address)) ==
          CUDA_SUCCESS);
  REQUIRE(source_context != nullptr);

  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  CUcontext caller_context = nullptr;
  REQUIRE(cuCtxGetCurrent(&caller_context) == CUDA_SUCCESS);
  REQUIRE(caller_context != nullptr);
  REQUIRE(caller_context != source_context);

  cv::cuda::GpuMat foreign_bgr(kContextHeight, kContextWidth, CV_8UC3);
  cv::cuda::GpuMat direct_bgr;
  auto direct_output = CudaFrame::Allocate(adapter.view(), source_context);
  const TestFrame host_output_template(kMwStreamerVideoPixelFormatNv12,
                                       kContextWidth, kContextHeight);
  auto direct_output_host =
      MakePinnedCopy(host_output_template.view(), source_context);
  CUstream direct_stream = nullptr;
  REQUIRE(cuCtxPushCurrent(source_context) == CUDA_SUCCESS);
  direct_bgr.create(kContextHeight, kContextWidth, CV_8UC3);
  REQUIRE(cuStreamCreate(&direct_stream, CU_STREAM_NON_BLOCKING) ==
          CUDA_SUCCESS);
  CUcontext popped_context = nullptr;
  REQUIRE(cuCtxPopCurrent(&popped_context) == CUDA_SUCCESS);
  REQUIRE(popped_context == source_context);

  CudaMatAdapter::ToBgr(adapter.view(), &direct_bgr, direct_stream);
  CudaMatAdapter::FromBgr(direct_bgr, adapter.view().color,
                          direct_output.view().buffer, direct_stream);
  direct_output.CopyTo(direct_output_host.view().buffer, direct_stream);
  CHECK_THROWS_AS(
      CudaMatAdapter::ToBgr(adapter.view(), &foreign_bgr, direct_stream),
      std::invalid_argument);
  CHECK_THROWS_AS(
      CudaMatAdapter::FromBgr(foreign_bgr, adapter.view().color,
                              direct_output.view().buffer, direct_stream),
      std::invalid_argument);
  CUcontext current_after_enqueue = nullptr;
  REQUIRE(cuCtxGetCurrent(&current_after_enqueue) == CUDA_SUCCESS);
  CHECK(current_after_enqueue == caller_context);
  REQUIRE(cuCtxPushCurrent(source_context) == CUDA_SUCCESS);
  REQUIRE(cuStreamSynchronize(direct_stream) == CUDA_SUCCESS);
  cv::Mat direct_bgr_host;
  direct_bgr.download(direct_bgr_host);
  direct_bgr.release();
  REQUIRE(cuStreamDestroy(direct_stream) == CUDA_SUCCESS);
  REQUIRE(cuCtxPopCurrent(&popped_context) == CUDA_SUCCESS);
  REQUIRE(popped_context == source_context);
  const auto direct_expected = HostMatAdapter::ToBgr(host_adapter.view());
  CheckMatsNear(direct_expected, direct_bgr_host, 3);
  const auto direct_round_trip =
      HostMatAdapter::ToBgr(direct_output_host.view());
  CheckMatsNear(direct_bgr_host, direct_round_trip, 4);
}

}  // namespace

#include "mw/opencv_adapter/cuda_mat_adapter.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>
#include <vector>

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/host_mat_adapter.h"
#include "mw/processor/internal/frame_adapter.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::ffmpeg::ThrowIfError;
using mw::streamer::opencv_adapter::CudaFrame;
using mw::streamer::opencv_adapter::CudaMatAdapter;
using mw::streamer::opencv_adapter::HostMatAdapter;
using mw::streamer::processor::internal::VideoFrameAdapter;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 16;

MwStreamerVideoColorInfo MakeColorInfo() {
  return {
      kMwStreamerColorRangeLimited,   kMwStreamerColorSpaceBt709,
      kMwStreamerColorPrimariesBt709, kMwStreamerColorTransferBt709,
      kMwStreamerChromaLocationLeft,
  };
}

class TestFrame final {
 public:
  explicit TestFrame(MwStreamerVideoPixelFormat format) {
    const bool is_16_bit = format == kMwStreamerVideoPixelFormatP010 ||
                           format == kMwStreamerVideoPixelFormatP016 ||
                           format == kMwStreamerVideoPixelFormatYuv444p16le;
    const bool is_planar = format == kMwStreamerVideoPixelFormatYuv444p ||
                           format == kMwStreamerVideoPixelFormatYuv444p16le;
    const std::uint32_t bytes_per_sample = is_16_bit ? 2 : 1;
    const std::uint32_t row_bytes = kWidth * bytes_per_sample;
    const std::uint32_t plane_count = is_planar ? 3 : 2;
    storage_.resize(plane_count);
    planes_.resize(plane_count);
    for (std::uint32_t index = 0; index < plane_count; ++index) {
      const std::uint32_t rows =
          is_planar || index == 0 ? kHeight : kHeight / 2;
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
         kMwStreamerVideoStorageLinear,
         format,
         kWidth,
         kHeight,
         {.linear = {planes_.data(), plane_count}}},
        MakeColorInfo(),
        {1234, 1, {1, 25}},
    };
  }

  const MwStreamerVideoFrameView& view() const { return view_; }

 private:
  template <typename Sample>
  void FillLuma(Sample black, Sample white) {
    auto* data = reinterpret_cast<Sample*>(storage_[0].data());
    for (std::uint32_t row = 0; row < kHeight; ++row) {
      const Sample value = row < kHeight / 2 ? black : white;
      std::fill_n(data + static_cast<std::size_t>(row) * kWidth, kWidth, value);
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
    CAPTURE(expected.at<cv::Vec<std::uint16_t, 3>>(0, 0),
            actual.at<cv::Vec<std::uint16_t, 3>>(0, 0));
  }
  CHECK(maximum_error <= tolerance);
}

void CheckP010IsQuantized(const CudaFrame& frame) {
  const auto host = frame.ToHost();
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

TEST_CASE("CudaMatAdapter在五种YUV格式和BGR之间同步转换") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  constexpr std::array kFormats = {
      kMwStreamerVideoPixelFormatNv12,
      kMwStreamerVideoPixelFormatP010,
      kMwStreamerVideoPixelFormatP016,
      kMwStreamerVideoPixelFormatYuv444p,
      kMwStreamerVideoPixelFormatYuv444p16le,
  };

  for (const auto format : kFormats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      const TestFrame source(format);
      const cv::Mat host_expected = HostMatAdapter::ToBgr(source.view());

      const cv::cuda::GpuMat gpu_bgr = CudaMatAdapter::ToBgr(source.view());
      cv::Mat gpu_actual;
      gpu_bgr.download(gpu_actual);
      CheckMatsNear(host_expected, gpu_actual,
                    host_expected.depth() == CV_8U ? 3 : 768);

      const auto cuda_source = CudaFrame::CopyFrom(source.view());
      const cv::cuda::GpuMat gpu_from_cuda =
          CudaMatAdapter::ToBgr(cuda_source.view());
      cv::Mat cuda_actual;
      gpu_from_cuda.download(cuda_actual);
      CheckMatsNear(host_expected, cuda_actual,
                    host_expected.depth() == CV_8U ? 3 : 768);

      const auto converted = CudaMatAdapter::FromBgr(gpu_bgr, source.view());
      CHECK(converted.view().buffer.memory_type == kMwStreamerMemoryCuda);
      CHECK(converted.view().buffer.pixel_format == format);
      CHECK(converted.view().timestamp.pts == 1234);
      if (format == kMwStreamerVideoPixelFormatP010) {
        CheckP010IsQuantized(converted);
      }

      const cv::cuda::GpuMat round_trip_gpu =
          CudaMatAdapter::ToBgr(converted.view());
      cv::Mat round_trip;
      round_trip_gpu.download(round_trip);
      CheckMatsNear(gpu_actual, round_trip,
                    host_expected.depth() == CV_8U ? 4 : 1024);
    }
  }
}

TEST_CASE("CudaMatAdapter拒绝HDR和不匹配的GpuMat") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  TestFrame source(kMwStreamerVideoPixelFormatNv12);
  auto hdr = source.view();
  hdr.color.transfer = kMwStreamerColorTransferSmpte2084;
  CHECK_THROWS_AS(CudaMatAdapter::ToBgr(hdr), std::invalid_argument);

  cv::cuda::GpuMat wrong_type(kHeight, kWidth, CV_16UC3);
  CHECK_THROWS_AS(CudaMatAdapter::FromBgr(wrong_type, source.view()),
                  std::invalid_argument);
}

TEST_CASE("CudaMatAdapter将FFmpeg CUDA View转换到调用方当前context") {
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
  const VideoFrameAdapter adapter(ffmpeg_cuda);

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

  const auto bgr = CudaMatAdapter::ToBgr(adapter.view());
  CUcontext bgr_context = nullptr;
  REQUIRE(cuPointerGetAttribute(&bgr_context, CU_POINTER_ATTRIBUTE_CONTEXT,
                                reinterpret_cast<CUdeviceptr>(bgr.data)) ==
          CUDA_SUCCESS);
  CHECK(bgr_context == caller_context);

  const auto converted = CudaMatAdapter::FromBgr(bgr, adapter.view());
  CUcontext converted_context = nullptr;
  REQUIRE(cuPointerGetAttribute(
              &converted_context, CU_POINTER_ATTRIBUTE_CONTEXT,
              static_cast<CUdeviceptr>(
                  converted.view().buffer.storage.linear.planes[0].address)) ==
          CUDA_SUCCESS);
  CHECK(converted_context == caller_context);
}

}  // namespace

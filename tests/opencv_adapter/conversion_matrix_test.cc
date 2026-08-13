#include <cuda_runtime_api.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/mat.hpp>
#include <vector>

#include "mw/opencv_adapter/cuda_frame.h"
#include "mw/opencv_adapter/cuda_mat_adapter.h"
#include "mw/opencv_adapter/host_frame.h"
#include "mw/opencv_adapter/host_mat_adapter.h"

namespace {

using mw::streamer::opencv_adapter::CudaFrame;
using mw::streamer::opencv_adapter::CudaMatAdapter;
using mw::streamer::opencv_adapter::HostFrame;
using mw::streamer::opencv_adapter::HostMatAdapter;

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 16;
constexpr std::uint32_t kPadding = 16;

constexpr std::array kFormats = {
    kMwStreamerVideoPixelFormatNv12,
    kMwStreamerVideoPixelFormatP010,
    kMwStreamerVideoPixelFormatP016,
    kMwStreamerVideoPixelFormatYuv420p,
    kMwStreamerVideoPixelFormatYuv422p,
    kMwStreamerVideoPixelFormatYuv444p,
    kMwStreamerVideoPixelFormatYuv420p10le,
    kMwStreamerVideoPixelFormatYuv422p10le,
    kMwStreamerVideoPixelFormatYuv444p10le,
    kMwStreamerVideoPixelFormatYuv444p16le,
};

constexpr std::array kColorSpaces = {
    kMwStreamerColorSpaceBt709,     kMwStreamerColorSpaceFcc,
    kMwStreamerColorSpaceBt470bg,   kMwStreamerColorSpaceSmpte170m,
    kMwStreamerColorSpaceSmpte240m, kMwStreamerColorSpaceBt2020Ncl,
};

constexpr std::array kColorRanges = {
    kMwStreamerColorRangeLimited,
    kMwStreamerColorRangeFull,
};

bool Is16Bit(MwStreamerVideoPixelFormat format) {
  return format == kMwStreamerVideoPixelFormatP010 ||
         format == kMwStreamerVideoPixelFormatP016 ||
         format == kMwStreamerVideoPixelFormatYuv420p10le ||
         format == kMwStreamerVideoPixelFormatYuv422p10le ||
         format == kMwStreamerVideoPixelFormatYuv444p10le ||
         format == kMwStreamerVideoPixelFormatYuv444p16le;
}

bool IsPlanar(MwStreamerVideoPixelFormat format) {
  return format == kMwStreamerVideoPixelFormatYuv420p ||
         format == kMwStreamerVideoPixelFormatYuv422p ||
         format == kMwStreamerVideoPixelFormatYuv444p ||
         format == kMwStreamerVideoPixelFormatYuv420p10le ||
         format == kMwStreamerVideoPixelFormatYuv422p10le ||
         format == kMwStreamerVideoPixelFormatYuv444p10le ||
         format == kMwStreamerVideoPixelFormatYuv444p16le;
}

bool IsPlanar10Bit(MwStreamerVideoPixelFormat format) {
  return format == kMwStreamerVideoPixelFormatYuv420p10le ||
         format == kMwStreamerVideoPixelFormatYuv422p10le ||
         format == kMwStreamerVideoPixelFormatYuv444p10le;
}

bool Is420(MwStreamerVideoPixelFormat format) {
  return format == kMwStreamerVideoPixelFormatNv12 ||
         format == kMwStreamerVideoPixelFormatP010 ||
         format == kMwStreamerVideoPixelFormatP016 ||
         format == kMwStreamerVideoPixelFormatYuv420p ||
         format == kMwStreamerVideoPixelFormatYuv420p10le;
}

bool IsHorizontallySubsampled(MwStreamerVideoPixelFormat format) {
  return Is420(format) || format == kMwStreamerVideoPixelFormatYuv422p ||
         format == kMwStreamerVideoPixelFormatYuv422p10le;
}

class ColoredYuvFrame final {
 public:
  ColoredYuvFrame(MwStreamerVideoPixelFormat format,
                  MwStreamerColorSpace color_space,
                  MwStreamerColorRange color_range)
      : format_(format) {
    const std::uint32_t bytes_per_sample = Is16Bit(format) ? 2 : 1;
    const std::uint32_t luma_row_bytes = kWidth * bytes_per_sample;
    const std::uint32_t plane_count = IsPlanar(format) ? 3 : 2;
    storage_.resize(plane_count);
    planes_.resize(plane_count);
    for (std::uint32_t index = 0; index < plane_count; ++index) {
      const bool chroma_plane = index != 0;
      const std::uint32_t row_bytes =
          chroma_plane && IsPlanar(format) && IsHorizontallySubsampled(format)
              ? luma_row_bytes / 2
              : luma_row_bytes;
      const std::uint32_t stride = row_bytes + kPadding;
      const std::uint32_t rows =
          chroma_plane && Is420(format) ? kHeight / 2 : kHeight;
      storage_[index].assign(static_cast<std::size_t>(stride) * rows, 0xcd);
      planes_[index] = {
          reinterpret_cast<std::uintptr_t>(storage_[index].data()),
          static_cast<std::int32_t>(stride), row_bytes, rows};
    }

    if (Is16Bit(format)) {
      Fill16Bit(color_range);
    } else {
      Fill8Bit(color_range);
    }
    view_ = {
        {kMwStreamerMemoryHost,
         kMwStreamerVideoStorageLinear,
         format,
         kWidth,
         kHeight,
         {.linear = {planes_.data(), plane_count}}},
        {color_range, color_space, kMwStreamerColorPrimariesBt709,
         kMwStreamerColorTransferBt709, kMwStreamerChromaLocationLeft},
        {1234, 1, {1, 25}},
    };
  }

  const MwStreamerVideoFrameView& view() const { return view_; }

  void ReverseRows() {
    for (std::size_t plane_index = 0; plane_index < planes_.size();
         ++plane_index) {
      auto& plane = planes_[plane_index];
      const std::size_t stride = plane.stride_bytes;
      std::vector<std::uint8_t> row(stride);
      for (std::uint32_t top = 0, bottom = plane.row_count - 1; top < bottom;
           ++top, --bottom) {
        auto* top_row = storage_[plane_index].data() + top * stride;
        auto* bottom_row = storage_[plane_index].data() + bottom * stride;
        std::memcpy(row.data(), top_row, stride);
        std::memcpy(top_row, bottom_row, stride);
        std::memcpy(bottom_row, row.data(), stride);
      }
      plane.address +=
          static_cast<std::uintptr_t>(plane.row_count - 1) * stride;
      plane.stride_bytes = -plane.stride_bytes;
    }
  }

  void FillFullRangeBlackAndWhite() {
    view_.color.range = kMwStreamerColorRangeFull;
    if (!Is16Bit(format_)) {
      FillLuma<std::uint8_t>(0, 255);
      FillChroma<std::uint8_t>(128, 128);
      return;
    }
    if (format_ == kMwStreamerVideoPixelFormatP010) {
      FillLuma<std::uint16_t>(0, 1023U << 6);
      FillChroma<std::uint16_t>(512U << 6, 512U << 6);
      return;
    }
    if (IsPlanar10Bit(format_)) {
      FillLuma<std::uint16_t>(0, 1023);
      FillChroma<std::uint16_t>(512, 512);
      return;
    }
    FillLuma<std::uint16_t>(0, 65535);
    FillChroma<std::uint16_t>(32768, 32768);
  }

 private:
  template <typename Sample>
  void FillPlane(std::size_t plane_index, Sample value) {
    const auto& plane = planes_[plane_index];
    for (std::uint32_t row = 0; row < plane.row_count; ++row) {
      auto* destination = reinterpret_cast<Sample*>(
          storage_[plane_index].data() +
          static_cast<std::size_t>(row) * plane.stride_bytes);
      std::fill_n(destination, plane.row_bytes / sizeof(Sample), value);
    }
  }

  template <typename Sample>
  void FillLuma(Sample dark, Sample bright) {
    const auto& plane = planes_[0];
    for (std::uint32_t row = 0; row < plane.row_count; ++row) {
      auto* destination = reinterpret_cast<Sample*>(
          storage_[0].data() +
          static_cast<std::size_t>(row) * plane.stride_bytes);
      const Sample value = row < plane.row_count / 2 ? dark : bright;
      std::fill_n(destination, plane.row_bytes / sizeof(Sample), value);
    }
  }

  template <typename Sample>
  void FillChroma(Sample u, Sample v) {
    if (IsPlanar(format_)) {
      FillPlane(1, u);
      FillPlane(2, v);
      return;
    }
    const auto& plane = planes_[1];
    for (std::uint32_t row = 0; row < plane.row_count; ++row) {
      auto* destination = reinterpret_cast<Sample*>(
          storage_[1].data() +
          static_cast<std::size_t>(row) * plane.stride_bytes);
      for (std::uint32_t column = 0; column < plane.row_bytes / sizeof(Sample);
           column += 2) {
        destination[column] = u;
        destination[column + 1] = v;
      }
    }
  }

  void Fill8Bit(MwStreamerColorRange range) {
    if (range == kMwStreamerColorRangeLimited) {
      FillLuma<std::uint8_t>(40, 200);
      FillChroma<std::uint8_t>(90, 180);
      return;
    }
    FillLuma<std::uint8_t>(30, 220);
    FillChroma<std::uint8_t>(70, 190);
  }

  void Fill16Bit(MwStreamerColorRange range) {
    if (format_ == kMwStreamerVideoPixelFormatP010) {
      if (range == kMwStreamerColorRangeLimited) {
        FillLuma<std::uint16_t>(160U << 6, 800U << 6);
        FillChroma<std::uint16_t>(300U << 6, 700U << 6);
      } else {
        FillLuma<std::uint16_t>(120U << 6, 880U << 6);
        FillChroma<std::uint16_t>(280U << 6, 720U << 6);
      }
      return;
    }
    if (IsPlanar10Bit(format_)) {
      if (range == kMwStreamerColorRangeLimited) {
        FillLuma<std::uint16_t>(160, 800);
        FillChroma<std::uint16_t>(300, 700);
      } else {
        FillLuma<std::uint16_t>(120, 880);
        FillChroma<std::uint16_t>(280, 720);
      }
      return;
    }
    if (range == kMwStreamerColorRangeLimited) {
      FillLuma<std::uint16_t>(40U << 8, 200U << 8);
      FillChroma<std::uint16_t>(90U << 8, 180U << 8);
      return;
    }
    FillLuma<std::uint16_t>(8000, 56000);
    FillChroma<std::uint16_t>(18000, 49000);
  }

  MwStreamerVideoPixelFormat format_;
  std::vector<std::vector<std::uint8_t>> storage_;
  std::vector<MwStreamerVideoPlaneView> planes_;
  MwStreamerVideoFrameView view_{};
};

int MatMaximumError(const cv::Mat& expected, const cv::Mat& actual) {
  if (actual.rows != expected.rows || actual.cols != expected.cols ||
      actual.type() != expected.type()) {
    return std::numeric_limits<int>::max();
  }
  int maximum_error = 0;
  const int value_count = expected.cols * expected.channels();
  if (expected.depth() == CV_8U) {
    for (int row = 0; row < expected.rows; ++row) {
      const auto* expected_row = expected.ptr<std::uint8_t>(row);
      const auto* actual_row = actual.ptr<std::uint8_t>(row);
      for (int index = 0; index < value_count; ++index) {
        maximum_error = std::max(
            maximum_error, std::abs(static_cast<int>(expected_row[index]) -
                                    static_cast<int>(actual_row[index])));
      }
    }
  } else {
    for (int row = 0; row < expected.rows; ++row) {
      const auto* expected_row = expected.ptr<std::uint16_t>(row);
      const auto* actual_row = actual.ptr<std::uint16_t>(row);
      for (int index = 0; index < value_count; ++index) {
        maximum_error = std::max(
            maximum_error, std::abs(static_cast<int>(expected_row[index]) -
                                    static_cast<int>(actual_row[index])));
      }
    }
  }
  return maximum_error;
}

int FrameMaximumError(const HostFrame& expected, const HostFrame& actual) {
  const auto& expected_linear = expected.view().buffer.storage.linear;
  const auto& actual_linear = actual.view().buffer.storage.linear;
  REQUIRE(actual_linear.plane_count == expected_linear.plane_count);
  int maximum_error = 0;
  const bool is_16_bit = Is16Bit(expected.view().buffer.pixel_format);
  for (std::uint32_t plane_index = 0; plane_index < expected_linear.plane_count;
       ++plane_index) {
    const auto& expected_plane = expected_linear.planes[plane_index];
    const auto& actual_plane = actual_linear.planes[plane_index];
    REQUIRE(actual_plane.row_bytes == expected_plane.row_bytes);
    REQUIRE(actual_plane.row_count == expected_plane.row_count);
    for (std::uint32_t row = 0; row < expected_plane.row_count; ++row) {
      const auto* expected_row = reinterpret_cast<const std::uint8_t*>(
          expected_plane.address +
          static_cast<std::uintptr_t>(row) * expected_plane.stride_bytes);
      const auto* actual_row = reinterpret_cast<const std::uint8_t*>(
          actual_plane.address +
          static_cast<std::uintptr_t>(row) * actual_plane.stride_bytes);
      if (!is_16_bit) {
        for (std::uint32_t index = 0; index < expected_plane.row_bytes;
             ++index) {
          maximum_error = std::max(
              maximum_error, std::abs(static_cast<int>(expected_row[index]) -
                                      static_cast<int>(actual_row[index])));
        }
        continue;
      }
      const auto* expected_values =
          reinterpret_cast<const std::uint16_t*>(expected_row);
      const auto* actual_values =
          reinterpret_cast<const std::uint16_t*>(actual_row);
      for (std::uint32_t index = 0; index < expected_plane.row_bytes / 2;
           ++index) {
        maximum_error = std::max(
            maximum_error, std::abs(static_cast<int>(expected_values[index]) -
                                    static_cast<int>(actual_values[index])));
      }
    }
  }
  return maximum_error;
}

bool Planar10BitSamplesAreValid(const HostFrame& frame) {
  const auto& linear = frame.view().buffer.storage.linear;
  for (std::uint32_t plane_index = 0; plane_index < linear.plane_count;
       ++plane_index) {
    const auto& plane = linear.planes[plane_index];
    for (std::uint32_t row = 0; row < plane.row_count; ++row) {
      const auto* values = reinterpret_cast<const std::uint16_t*>(
          plane.address +
          static_cast<std::uintptr_t>(row) * plane.stride_bytes);
      for (std::uint32_t index = 0; index < plane.row_bytes / 2; ++index) {
        if (values[index] > 1023) {
          return false;
        }
      }
    }
  }
  return true;
}

cv::Mat MakeUniformBgr(MwStreamerVideoPixelFormat format) {
  if (Is16Bit(format)) {
    return cv::Mat(kHeight, kWidth, CV_16UC3, cv::Scalar(50000, 18000, 35000));
  }
  return cv::Mat(kHeight, kWidth, CV_8UC3, cv::Scalar(195, 70, 140));
}

TEST_CASE("Host和CUDA转换覆盖全部颜色矩阵与范围") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  for (const auto format : kFormats) {
    for (const auto color_space : kColorSpaces) {
      for (const auto color_range : kColorRanges) {
        DYNAMIC_SECTION("format=" << static_cast<int>(format) << " space="
                                  << static_cast<int>(color_space) << " range="
                                  << static_cast<int>(color_range)) {
          const ColoredYuvFrame source(format, color_space, color_range);
          const cv::Mat cpu_bgr = HostMatAdapter::ToBgr(source.view());

          const cv::cuda::GpuMat gpu_bgr = CudaMatAdapter::ToBgr(source.view());
          cv::Mat downloaded_bgr;
          gpu_bgr.download(downloaded_bgr);
          const int forward_error = MatMaximumError(cpu_bgr, downloaded_bgr);
          CAPTURE(forward_error);
          CHECK(forward_error <= (Is16Bit(format) ? 1024 : 4));

          const auto cuda_source = CudaFrame::CopyFrom(source.view());
          const auto gpu_from_cuda = CudaMatAdapter::ToBgr(cuda_source.view());
          cv::Mat downloaded_from_cuda;
          gpu_from_cuda.download(downloaded_from_cuda);
          CHECK(MatMaximumError(downloaded_bgr, downloaded_from_cuda) == 0);

          const cv::Mat input_bgr = MakeUniformBgr(format);
          const HostFrame cpu_yuv =
              HostMatAdapter::FromBgr(input_bgr, source.view());
          cv::cuda::GpuMat input_gpu;
          input_gpu.upload(input_bgr);
          const CudaFrame gpu_yuv =
              CudaMatAdapter::FromBgr(input_gpu, source.view());
          const HostFrame downloaded_yuv = gpu_yuv.ToHost();
          const int reverse_error = FrameMaximumError(cpu_yuv, downloaded_yuv);
          CAPTURE(reverse_error);
          CHECK(reverse_error <=
                (IsPlanar10Bit(format) ? 16 : (Is16Bit(format) ? 1024 : 4)));
          if (IsPlanar10Bit(format)) {
            CHECK(Planar10BitSamplesAreValid(downloaded_yuv));
          }

          const cv::Mat cpu_round_trip = HostMatAdapter::ToBgr(cpu_yuv.view());
          const auto gpu_round_trip = CudaMatAdapter::ToBgr(gpu_yuv.view());
          cv::Mat downloaded_round_trip;
          gpu_round_trip.download(downloaded_round_trip);
          CHECK(MatMaximumError(cpu_round_trip, downloaded_round_trip) <=
                (Is16Bit(format) ? 1536 : 6));
        }
      }
    }
  }
}

TEST_CASE("CudaMatAdapter正确重采样YUV422色度") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  constexpr std::array formats = {kMwStreamerVideoPixelFormatYuv422p,
                                  kMwStreamerVideoPixelFormatYuv422p10le};
  for (const auto format : formats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      const ColoredYuvFrame prototype(format, kMwStreamerColorSpaceBt709,
                                      kMwStreamerColorRangeLimited);
      const int mat_type = Is16Bit(format) ? CV_16UC3 : CV_8UC3;
      cv::Mat input(kHeight, kWidth, mat_type);
      for (int row = 0; row < input.rows; ++row) {
        for (int column = 0; column < input.cols; ++column) {
          if (Is16Bit(format)) {
            input.at<cv::Vec3w>(row, column) = {
                static_cast<std::uint16_t>(column * 900),
                static_cast<std::uint16_t>(8000 + column * 700),
                static_cast<std::uint16_t>(58000 - column * 600)};
          } else {
            input.at<cv::Vec3b>(row, column) = {
                static_cast<std::uint8_t>(column * 4),
                static_cast<std::uint8_t>(32 + column * 3),
                static_cast<std::uint8_t>(192 - column * 2)};
          }
        }
      }

      const HostFrame cpu = HostMatAdapter::FromBgr(input, prototype.view());
      cv::cuda::GpuMat gpu_input;
      gpu_input.upload(input);
      const HostFrame gpu =
          CudaMatAdapter::FromBgr(gpu_input, prototype.view()).ToHost();
      CHECK(FrameMaximumError(cpu, gpu) <= (Is16Bit(format) ? 16 : 4));
      if (IsPlanar10Bit(format)) {
        CHECK(Planar10BitSamplesAreValid(gpu));
      }
    }
  }
}

TEST_CASE("Host和CUDA反向转换接受带padding的Mat ROI") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  for (const auto format : kFormats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      const ColoredYuvFrame prototype(format, kMwStreamerColorSpaceBt709,
                                      kMwStreamerColorRangeLimited);
      const int mat_type = Is16Bit(format) ? CV_16UC3 : CV_8UC3;
      cv::Mat parent(kHeight, kWidth + 16, mat_type);
      cv::Mat roi = parent(cv::Rect(7, 0, kWidth, kHeight));
      if (Is16Bit(format)) {
        roi.setTo(cv::Scalar(51000, 17000, 33000));
      } else {
        roi.setTo(cv::Scalar(200, 65, 135));
      }
      REQUIRE_FALSE(roi.isContinuous());

      const HostFrame host_from_roi =
          HostMatAdapter::FromBgr(roi, prototype.view());
      const HostFrame host_from_clone =
          HostMatAdapter::FromBgr(roi.clone(), prototype.view());
      CHECK(FrameMaximumError(host_from_clone, host_from_roi) == 0);

      cv::cuda::GpuMat gpu_parent(kHeight, kWidth + 16, mat_type);
      cv::cuda::GpuMat gpu_roi = gpu_parent(cv::Rect(7, 0, kWidth, kHeight));
      gpu_roi.upload(roi);
      REQUIRE(gpu_roi.step > gpu_roi.cols * gpu_roi.elemSize());
      const CudaFrame cuda_from_roi =
          CudaMatAdapter::FromBgr(gpu_roi, prototype.view());
      const HostFrame downloaded = cuda_from_roi.ToHost();
      CHECK(FrameMaximumError(host_from_roi, downloaded) <=
            (Is16Bit(format) ? 1024 : 4));
    }
  }
}

TEST_CASE("Host和CUDA正向转换接受负stride的Host View") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  for (const auto format : kFormats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      const ColoredYuvFrame positive(format, kMwStreamerColorSpaceBt709,
                                     kMwStreamerColorRangeLimited);
      ColoredYuvFrame negative(format, kMwStreamerColorSpaceBt709,
                               kMwStreamerColorRangeLimited);
      negative.ReverseRows();

      const cv::Mat expected = HostMatAdapter::ToBgr(positive.view());
      const cv::Mat host_actual = HostMatAdapter::ToBgr(negative.view());
      CHECK(MatMaximumError(expected, host_actual) == 0);

      const auto gpu_actual = CudaMatAdapter::ToBgr(negative.view());
      cv::Mat downloaded;
      gpu_actual.download(downloaded);
      CHECK(MatMaximumError(expected, downloaded) <=
            (Is16Bit(format) ? 1024 : 4));
    }
  }
}

TEST_CASE("Host和CUDA转换正确处理full range端点") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  for (const auto format : kFormats) {
    DYNAMIC_SECTION("format=" << static_cast<int>(format)) {
      ColoredYuvFrame source(format, kMwStreamerColorSpaceBt709,
                             kMwStreamerColorRangeFull);
      source.FillFullRangeBlackAndWhite();
      const cv::Mat host = HostMatAdapter::ToBgr(source.view());
      const auto gpu = CudaMatAdapter::ToBgr(source.view());
      cv::Mat downloaded;
      gpu.download(downloaded);
      CHECK(MatMaximumError(host, downloaded) <= (Is16Bit(format) ? 1024 : 4));

      if (!Is16Bit(format)) {
        const auto black = downloaded.at<cv::Vec3b>(0, 0);
        const auto white =
            downloaded.at<cv::Vec3b>(downloaded.rows - 1, downloaded.cols - 1);
        for (int channel = 0; channel < 3; ++channel) {
          CHECK(black[channel] <= 1);
          CHECK(white[channel] >= 250);
        }
      } else {
        const auto black = downloaded.at<cv::Vec<std::uint16_t, 3>>(0, 0);
        const auto white = downloaded.at<cv::Vec<std::uint16_t, 3>>(
            downloaded.rows - 1, downloaded.cols - 1);
        for (int channel = 0; channel < 3; ++channel) {
          CHECK(black[channel] <= 64);
          CHECK(white[channel] >= 64500);
        }
      }
    }
  }
}

TEST_CASE("CudaMatAdapter支持多个调用线程同步转换") {
  constexpr int kWorkerCount = 8;
  std::array<std::future<int>, kWorkerCount> workers;
  for (int index = 0; index < kWorkerCount; ++index) {
    workers[index] = std::async(std::launch::async, [index] {
      if (cudaSetDevice(0) != cudaSuccess) {
        return -1;
      }
      const auto format = kFormats[index % kFormats.size()];
      const auto color_space = kColorSpaces[index % kColorSpaces.size()];
      const auto color_range = kColorRanges[index % kColorRanges.size()];
      const ColoredYuvFrame source(format, color_space, color_range);
      const cv::Mat expected = HostMatAdapter::ToBgr(source.view());
      const auto gpu_bgr = CudaMatAdapter::ToBgr(source.view());
      cv::Mat actual;
      gpu_bgr.download(actual);
      return MatMaximumError(expected, actual);
    });
  }
  for (int index = 0; index < kWorkerCount; ++index) {
    CAPTURE(index);
    CHECK(workers[index].get() <=
          (Is16Bit(kFormats[index % kFormats.size()]) ? 1024 : 4));
  }
}

TEST_CASE("Host和CUDA Mat Adapter拒绝无效格式元数据和布局") {
  REQUIRE(cudaSetDevice(0) == cudaSuccess);
  const ColoredYuvFrame source(kMwStreamerVideoPixelFormatNv12,
                               kMwStreamerColorSpaceBt709,
                               kMwStreamerColorRangeLimited);

  const auto check_to_bgr_rejected = [](MwStreamerVideoFrameView invalid) {
    CHECK_THROWS_AS(HostMatAdapter::ToBgr(invalid), std::invalid_argument);
    CHECK_THROWS_AS(CudaMatAdapter::ToBgr(invalid), std::invalid_argument);
  };

  auto invalid = source.view();
  invalid.buffer.pixel_format = kMwStreamerVideoPixelFormatYuv420p;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.buffer.memory_type = static_cast<MwStreamerMemoryType>(999);
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.buffer.storage_type = kMwStreamerVideoStorageNativeSurface;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.buffer.width = 0;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.buffer.height = 0;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.color.space = kMwStreamerColorSpaceUnknown;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.color.range = kMwStreamerColorRangeUnknown;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.color.transfer = kMwStreamerColorTransferUnknown;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.color.transfer = kMwStreamerColorTransferSmpte2084;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.color.transfer = kMwStreamerColorTransferAribStdB67;
  check_to_bgr_rejected(invalid);
  invalid = source.view();
  invalid.buffer.storage.linear = {nullptr, 0};
  check_to_bgr_rejected(invalid);

  std::array<MwStreamerVideoPlaneView, 2> planes = {
      source.view().buffer.storage.linear.planes[0],
      source.view().buffer.storage.linear.planes[1]};
  invalid = source.view();
  invalid.buffer.storage.linear = {planes.data(), 1};
  check_to_bgr_rejected(invalid);
  invalid.buffer.storage.linear.plane_count = 2;
  planes[0].address = 0;
  check_to_bgr_rejected(invalid);
  planes[0] = source.view().buffer.storage.linear.planes[0];
  planes[0].stride_bytes = 1;
  check_to_bgr_rejected(invalid);
  planes[0] = source.view().buffer.storage.linear.planes[0];
  planes[0].row_bytes -= 1;
  check_to_bgr_rejected(invalid);
  planes[0] = source.view().buffer.storage.linear.planes[0];
  planes[0].row_count -= 1;
  check_to_bgr_rejected(invalid);

  const cv::Mat valid_bgr = MakeUniformBgr(kMwStreamerVideoPixelFormatNv12);
  CHECK_THROWS_AS(HostMatAdapter::FromBgr(cv::Mat(), source.view()),
                  std::invalid_argument);
  CHECK_THROWS_AS(HostMatAdapter::FromBgr(cv::Mat(kHeight, kWidth, CV_16UC3),
                                          source.view()),
                  std::invalid_argument);
  CHECK_THROWS_AS(HostMatAdapter::FromBgr(cv::Mat(kHeight, kWidth - 1, CV_8UC3),
                                          source.view()),
                  std::invalid_argument);

  cv::cuda::GpuMat valid_gpu;
  valid_gpu.upload(valid_bgr);
  CHECK_THROWS_AS(CudaMatAdapter::FromBgr(cv::cuda::GpuMat(), source.view()),
                  std::invalid_argument);
  CHECK_THROWS_AS(
      CudaMatAdapter::FromBgr(cv::cuda::GpuMat(kHeight, kWidth, CV_16UC3),
                              source.view()),
      std::invalid_argument);
  CHECK_THROWS_AS(
      CudaMatAdapter::FromBgr(cv::cuda::GpuMat(kHeight, kWidth - 1, CV_8UC3),
                              source.view()),
      std::invalid_argument);
  CHECK_NOTHROW(HostMatAdapter::FromBgr(valid_bgr, source.view()));
  CHECK_NOTHROW(CudaMatAdapter::FromBgr(valid_gpu, source.view()));
}

}  // namespace

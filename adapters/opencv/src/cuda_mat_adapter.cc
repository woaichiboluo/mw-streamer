#include "mw/opencv_adapter/cuda_mat_adapter.h"

#include <cuda_runtime_api.h>
#include <nppcore.h>
#include <nppi_arithmetic_and_logical_operations.h>
#include <nppi_color_conversion.h>
#include <nppi_data_exchange_and_initialization.h>
#include <nppi_geometry_transforms.h>
#include <nppi_threshold_and_compare_operations.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace mw::streamer::opencv_adapter {
namespace {

enum class PixelLayout {
  kSemiPlanar420,
  kPlanar420,
  kPlanar422,
  kPlanar444,
};

struct PixelFormatInfo {
  int mat_type = -1;
  int value_bits = 0;
  std::uint32_t bytes_per_sample = 0;
  std::uint32_t plane_count = 0;
  PixelLayout layout = PixelLayout::kSemiPlanar420;
  bool p010 = false;
};

struct ColorCoefficients {
  float red = 0.0F;
  float blue = 0.0F;
};

using ColorMatrix = std::array<std::array<Npp32f, 4>, 3>;

const Npp32f (*MatrixData(const ColorMatrix& matrix))[4] {
  static_assert(sizeof(ColorMatrix) == sizeof(Npp32f) * 12);
  return reinterpret_cast<const Npp32f(*)[4]>(matrix.data());
}

void ThrowIfCudaError(cudaError_t result, const char* operation) {
  if (result == cudaSuccess) {
    return;
  }
  throw std::runtime_error(std::string(operation) +
                           "失败: " + cudaGetErrorName(result));
}

void ThrowIfNppError(NppStatus status, const char* operation) {
  if (status == NPP_SUCCESS) {
    return;
  }
  throw std::runtime_error(std::string(operation) +
                           "失败: NPP status=" + std::to_string(status));
}

PixelFormatInfo GetPixelFormatInfo(MwStreamerVideoPixelFormat format) {
  switch (format) {
    case kMwStreamerVideoPixelFormatNv12:
      return {CV_8UC3, 8, 1, 2, PixelLayout::kSemiPlanar420, false};
    case kMwStreamerVideoPixelFormatP010:
      return {CV_16UC3, 16, 2, 2, PixelLayout::kSemiPlanar420, true};
    case kMwStreamerVideoPixelFormatP016:
      return {CV_16UC3, 16, 2, 2, PixelLayout::kSemiPlanar420, false};
    case kMwStreamerVideoPixelFormatYuv420p:
      return {CV_8UC3, 8, 1, 3, PixelLayout::kPlanar420, false};
    case kMwStreamerVideoPixelFormatYuv422p:
      return {CV_8UC3, 8, 1, 3, PixelLayout::kPlanar422, false};
    case kMwStreamerVideoPixelFormatYuv444p:
      return {CV_8UC3, 8, 1, 3, PixelLayout::kPlanar444, false};
    case kMwStreamerVideoPixelFormatYuv420p10le:
      return {CV_16UC3, 10, 2, 3, PixelLayout::kPlanar420, false};
    case kMwStreamerVideoPixelFormatYuv422p10le:
      return {CV_16UC3, 10, 2, 3, PixelLayout::kPlanar422, false};
    case kMwStreamerVideoPixelFormatYuv444p10le:
      return {CV_16UC3, 10, 2, 3, PixelLayout::kPlanar444, false};
    case kMwStreamerVideoPixelFormatYuv444p16le:
      return {CV_16UC3, 16, 2, 3, PixelLayout::kPlanar444, false};
    default:
      throw std::invalid_argument("CudaMatAdapter不支持该视频像素格式");
  }
}

bool IsSemiPlanar(const PixelFormatInfo& format) {
  return format.layout == PixelLayout::kSemiPlanar420;
}

bool IsSubsampledHorizontally(const PixelFormatInfo& format) {
  return format.layout == PixelLayout::kPlanar420 ||
         format.layout == PixelLayout::kPlanar422;
}

bool IsSubsampledVertically(const PixelFormatInfo& format) {
  return format.layout == PixelLayout::kSemiPlanar420 ||
         format.layout == PixelLayout::kPlanar420;
}

ColorCoefficients GetColorCoefficients(MwStreamerColorSpace colorspace) {
  switch (colorspace) {
    case kMwStreamerColorSpaceBt709:
      return {0.2126F, 0.0722F};
    case kMwStreamerColorSpaceFcc:
      return {0.30F, 0.11F};
    case kMwStreamerColorSpaceBt470bg:
    case kMwStreamerColorSpaceSmpte170m:
      return {0.299F, 0.114F};
    case kMwStreamerColorSpaceSmpte240m:
      return {0.212F, 0.087F};
    case kMwStreamerColorSpaceBt2020Ncl:
      return {0.2627F, 0.0593F};
    default:
      throw std::invalid_argument("CudaMatAdapter不支持该视频颜色空间");
  }
}

void ValidateTransfer(MwStreamerColorTransfer transfer) {
  if (transfer == kMwStreamerColorTransferUnknown) {
    throw std::invalid_argument("CudaMatAdapter不支持未知视频传递函数");
  }
  if (transfer == kMwStreamerColorTransferSmpte2084 ||
      transfer == kMwStreamerColorTransferAribStdB67) {
    throw std::invalid_argument("CudaMatAdapter暂不支持PQ或HLG视频");
  }
}

PixelFormatInfo ValidatePrototype(const MwStreamerVideoFrameView& prototype) {
  const auto format = GetPixelFormatInfo(prototype.buffer.pixel_format);
  const auto& buffer = prototype.buffer;
  if (buffer.memory_type != kMwStreamerMemoryHost &&
      buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("CudaMatAdapter收到未知视频内存类型");
  }
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("CudaMatAdapter只接受linear视频存储");
  }
  if (buffer.width == 0 || buffer.height == 0 ||
      buffer.width >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      buffer.height >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("CudaMatAdapter要求有效的视频宽高");
  }
  if ((IsSemiPlanar(format) || IsSubsampledHorizontally(format)) &&
      buffer.width % 2 != 0) {
    throw std::invalid_argument("CudaMatAdapter要求色度降采样视频具有偶数宽度");
  }
  if (IsSubsampledVertically(format) && buffer.height % 2 != 0) {
    throw std::invalid_argument("CudaMatAdapter要求4:2:0视频具有偶数高度");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes || linear.plane_count != format.plane_count) {
    throw std::invalid_argument("CudaMatAdapter收到错误的视频平面数量");
  }
  const std::uint64_t expected_row_bytes_64 =
      static_cast<std::uint64_t>(buffer.width) * format.bytes_per_sample;
  const std::uint64_t expected_bgr_row_bytes = expected_row_bytes_64 * 3;
  if (expected_row_bytes_64 > std::numeric_limits<std::uint32_t>::max() ||
      expected_bgr_row_bytes > std::numeric_limits<int>::max()) {
    throw std::invalid_argument("CudaMatAdapter视频行宽超过NPP限制");
  }
  const auto expected_row_bytes =
      static_cast<std::uint32_t>(expected_row_bytes_64);
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    const bool chroma_plane = index != 0;
    const std::uint32_t expected_rows =
        chroma_plane && IsSubsampledVertically(format) ? buffer.height / 2
                                                       : buffer.height;
    const std::uint32_t plane_row_bytes =
        chroma_plane && !IsSemiPlanar(format) &&
                IsSubsampledHorizontally(format)
            ? expected_row_bytes / 2
            : expected_row_bytes;
    const auto stride = static_cast<std::int64_t>(plane.stride_bytes);
    const auto absolute_stride = stride < 0 ? -stride : stride;
    if (plane.address == 0 || plane.stride_bytes == 0 ||
        plane.row_bytes != plane_row_bytes ||
        plane.row_count != expected_rows || absolute_stride < plane_row_bytes ||
        (buffer.memory_type == kMwStreamerMemoryCuda && stride < 0)) {
      throw std::invalid_argument("CudaMatAdapter收到无效的视频平面布局");
    }
  }

  GetColorCoefficients(prototype.color.space);
  if (prototype.color.range != kMwStreamerColorRangeLimited &&
      prototype.color.range != kMwStreamerColorRangeFull) {
    throw std::invalid_argument("CudaMatAdapter不支持未知视频颜色范围");
  }
  ValidateTransfer(prototype.color.transfer);
  return format;
}

float MaxValue(int value_bits) {
  return static_cast<float>((1U << value_bits) - 1U);
}

struct ColorLevels {
  float maximum;
  float y_offset;
  float y_range;
  float chroma_center;
  float chroma_range;
};

ColorLevels GetColorLevels(int value_bits, MwStreamerColorRange range) {
  const float scale = static_cast<float>(1U << (value_bits - 8));
  const float maximum = MaxValue(value_bits);
  if (range == kMwStreamerColorRangeLimited) {
    return {maximum, 16.0F * scale, 219.0F * scale, 128.0F * scale,
            224.0F * scale};
  }
  return {maximum, 0.0F, maximum, static_cast<float>(1U << (value_bits - 1)),
          maximum};
}

ColorMatrix MakeForwardMatrix(const MwStreamerVideoFrameView& prototype,
                              const PixelFormatInfo& format) {
  const auto coefficients = GetColorCoefficients(prototype.color.space);
  const float red = coefficients.red;
  const float blue = coefficients.blue;
  const float green = 1.0F - red - blue;
  const auto levels = GetColorLevels(format.value_bits, prototype.color.range);
  const float bgr_maximum = format.mat_type == CV_8UC3 ? 255.0F : 65535.0F;
  const float y_scale = levels.y_range / bgr_maximum;
  const float chroma_scale = levels.chroma_range / bgr_maximum;
  const float rounding = format.p010 ? 32.0F : 0.0F;
  return {{{blue * y_scale, green * y_scale, red * y_scale,
            levels.y_offset + rounding},
           {0.5F * chroma_scale, -green * chroma_scale / (2.0F * (1.0F - blue)),
            -red * chroma_scale / (2.0F * (1.0F - blue)),
            levels.chroma_center + rounding},
           {-blue * chroma_scale / (2.0F * (1.0F - red)),
            -green * chroma_scale / (2.0F * (1.0F - red)), 0.5F * chroma_scale,
            levels.chroma_center + rounding}}};
}

ColorMatrix MakeInverseMatrix(const MwStreamerVideoFrameView& prototype,
                              const PixelFormatInfo& format) {
  const auto coefficients = GetColorCoefficients(prototype.color.space);
  const float red = coefficients.red;
  const float blue = coefficients.blue;
  const float green = 1.0F - red - blue;
  const auto levels = GetColorLevels(format.value_bits, prototype.color.range);
  const float bgr_maximum = format.mat_type == CV_8UC3 ? 255.0F : 65535.0F;
  const float y_scale = bgr_maximum / levels.y_range;
  const float chroma_scale = bgr_maximum / levels.chroma_range;
  const float blue_chroma = 2.0F * (1.0F - blue) * chroma_scale;
  const float red_chroma = 2.0F * (1.0F - red) * chroma_scale;
  const float green_blue = -blue * 2.0F * (1.0F - blue) / green * chroma_scale;
  const float green_red = -red * 2.0F * (1.0F - red) / green * chroma_scale;
  return {{{y_scale, blue_chroma, 0.0F,
            -y_scale * levels.y_offset - blue_chroma * levels.chroma_center},
           {y_scale, green_blue, green_red,
            -y_scale * levels.y_offset -
                (green_blue + green_red) * levels.chroma_center},
           {y_scale, 0.0F, red_chroma,
            -y_scale * levels.y_offset - red_chroma * levels.chroma_center}}};
}

NppStreamContext MakeNppStreamContext() {
  NppStreamContext context{};
  ThrowIfNppError(nppGetStreamContext(&context), "获取NPP stream context");
  context.hStream = nullptr;
  return context;
}

void ValidateNppStep(std::int32_t step) {
  if (step <= 0) {
    throw std::invalid_argument("CudaMatAdapter不支持非正数CUDA stride");
  }
}

void ValidateGpuMat(const cv::cuda::GpuMat& source,
                    const MwStreamerVideoFrameView& prototype,
                    const PixelFormatInfo& format) {
  if (source.empty() ||
      source.cols != static_cast<int>(prototype.buffer.width) ||
      source.rows != static_cast<int>(prototype.buffer.height) ||
      source.type() != format.mat_type ||
      source.step > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("OpenCV CUDA BGR GpuMat与视频原型不匹配");
  }
}

template <typename Sample>
void ConvertSemiPlanarToBgr(const MwStreamerVideoFrameView& source,
                            cv::cuda::GpuMat* destination,
                            const PixelFormatInfo& format,
                            const NppStreamContext& stream_context) {
  const auto& linear = source.buffer.storage.linear;
  const Sample* source_planes[2] = {
      reinterpret_cast<const Sample*>(linear.planes[0].address),
      reinterpret_cast<const Sample*>(linear.planes[1].address)};
  int source_steps[2] = {linear.planes[0].stride_bytes,
                         linear.planes[1].stride_bytes};
  const NppiSize size = {static_cast<int>(source.buffer.width),
                         static_cast<int>(source.buffer.height)};
  const auto matrix = MakeInverseMatrix(source, format);
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiNV12ToRGB_8u_ColorTwist32f_P2C3R_Ctx(
        source_planes, source_steps, destination->ptr<Npp8u>(),
        static_cast<int>(destination->step), size, MatrixData(matrix),
        stream_context);
  } else {
    status = nppiNV12ToRGB_16u_ColorTwist32f_P2C3R_Ctx(
        source_planes, source_steps, destination->ptr<Npp16u>(),
        static_cast<int>(destination->step), size, MatrixData(matrix),
        stream_context);
  }
  ThrowIfNppError(status, "NPP转换半平面YUV到BGR");
}

template <typename Sample>
void ConvertPlanar444ToBgr(const MwStreamerVideoFrameView& source,
                           cv::cuda::GpuMat* destination,
                           const PixelFormatInfo& format,
                           const NppStreamContext& stream_context) {
  const auto& linear = source.buffer.storage.linear;
  if (linear.planes[0].stride_bytes != linear.planes[1].stride_bytes ||
      linear.planes[0].stride_bytes != linear.planes[2].stride_bytes) {
    throw std::invalid_argument(
        "CudaMatAdapter要求YUV444三个平面具有相同stride");
  }
  cv::cuda::GpuMat packed(static_cast<int>(source.buffer.height),
                          static_cast<int>(source.buffer.width),
                          sizeof(Sample) == 1 ? CV_8UC3 : CV_16UC3);
  const Sample* source_planes[3] = {
      reinterpret_cast<const Sample*>(linear.planes[0].address),
      reinterpret_cast<const Sample*>(linear.planes[1].address),
      reinterpret_cast<const Sample*>(linear.planes[2].address)};
  const NppiSize size = {static_cast<int>(source.buffer.width),
                         static_cast<int>(source.buffer.height)};
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiCopy_8u_P3C3R_Ctx(
        source_planes, linear.planes[0].stride_bytes, packed.ptr<Npp8u>(),
        static_cast<int>(packed.step), size, stream_context);
  } else {
    status = nppiCopy_16u_P3C3R_Ctx(
        source_planes, linear.planes[0].stride_bytes, packed.ptr<Npp16u>(),
        static_cast<int>(packed.step), size, stream_context);
  }
  ThrowIfNppError(status, "NPP合并YUV444平面");

  const auto matrix = MakeInverseMatrix(source, format);
  if constexpr (sizeof(Sample) == 1) {
    status = nppiColorTwist32f_8u_C3R_Ctx(
        packed.ptr<Npp8u>(), static_cast<int>(packed.step),
        destination->ptr<Npp8u>(), static_cast<int>(destination->step), size,
        MatrixData(matrix), stream_context);
  } else {
    status = nppiColorTwist32f_16u_C3R_Ctx(
        packed.ptr<Npp16u>(), static_cast<int>(packed.step),
        destination->ptr<Npp16u>(), static_cast<int>(destination->step), size,
        MatrixData(matrix), stream_context);
  }
  ThrowIfNppError(status, "NPP转换YUV444到BGR");
}

template <typename Sample>
void ConvertSubsampledPlanarToBgr(const MwStreamerVideoFrameView& source,
                                  cv::cuda::GpuMat* destination,
                                  const PixelFormatInfo& format,
                                  const NppStreamContext& stream_context) {
  const auto& linear = source.buffer.storage.linear;
  const Sample* source_planes[3] = {
      reinterpret_cast<const Sample*>(linear.planes[0].address),
      reinterpret_cast<const Sample*>(linear.planes[1].address),
      reinterpret_cast<const Sample*>(linear.planes[2].address)};
  int source_steps[3] = {linear.planes[0].stride_bytes,
                         linear.planes[1].stride_bytes,
                         linear.planes[2].stride_bytes};
  const NppiSize size = {static_cast<int>(source.buffer.width),
                         static_cast<int>(source.buffer.height)};
  const auto matrix = MakeInverseMatrix(source, format);
  NppStatus status;
  if (format.layout == PixelLayout::kPlanar420) {
    if constexpr (sizeof(Sample) == 1) {
      status = nppiYUV420ToRGB_8u_ColorTwist32f_P3C3R_Ctx(
          source_planes, source_steps, destination->ptr<Npp8u>(),
          static_cast<int>(destination->step), size, MatrixData(matrix),
          stream_context);
    } else {
      status = nppiYUV420ToRGB_16u_ColorTwist32f_P3C3R_Ctx(
          source_planes, source_steps, destination->ptr<Npp16u>(),
          static_cast<int>(destination->step), size, MatrixData(matrix),
          stream_context);
    }
  } else if constexpr (sizeof(Sample) == 1) {
    status = nppiYUV422ToRGB_8u_ColorTwist32f_P3C3R_Ctx(
        source_planes, source_steps, destination->ptr<Npp8u>(),
        static_cast<int>(destination->step), size, MatrixData(matrix),
        stream_context);
  } else {
    status = nppiYUV422ToRGB_16u_ColorTwist32f_P3C3R_Ctx(
        source_planes, source_steps, destination->ptr<Npp16u>(),
        static_cast<int>(destination->step), size, MatrixData(matrix),
        stream_context);
  }
  ThrowIfNppError(status, "NPP转换planar YUV到BGR");
}

template <typename Sample>
void ConvertBgrToSemiPlanar(const cv::cuda::GpuMat& source,
                            MwStreamerVideoFrameView* destination,
                            const PixelFormatInfo& format,
                            const NppStreamContext& stream_context) {
  auto& linear = destination->buffer.storage.linear;
  Sample* destination_planes[2] = {
      reinterpret_cast<Sample*>(linear.planes[0].address),
      reinterpret_cast<Sample*>(linear.planes[1].address)};
  int destination_steps[2] = {linear.planes[0].stride_bytes,
                              linear.planes[1].stride_bytes};
  const NppiSize size = {static_cast<int>(destination->buffer.width),
                         static_cast<int>(destination->buffer.height)};
  const auto matrix = MakeForwardMatrix(*destination, format);
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiRGBToNV12_8u_ColorTwist32f_C3P2R_Ctx(
        source.ptr<Npp8u>(), static_cast<int>(source.step), destination_planes,
        destination_steps, size, MatrixData(matrix), stream_context);
  } else {
    status = nppiRGBToNV12_16u_ColorTwist32f_C3P2R_Ctx(
        source.ptr<Npp16u>(), static_cast<int>(source.step), destination_planes,
        destination_steps, size, MatrixData(matrix), stream_context);
  }
  ThrowIfNppError(status, "NPP转换BGR到半平面YUV");

  if (format.p010) {
    const Npp16u mask = 0xffc0U;
    ThrowIfNppError(nppiAndC_16u_C1IR_Ctx(
                        mask, reinterpret_cast<Npp16u*>(destination_planes[0]),
                        destination_steps[0], size, stream_context),
                    "量化P010亮度平面");
    const NppiSize chroma_size = {size.width, size.height / 2};
    ThrowIfNppError(nppiAndC_16u_C1IR_Ctx(
                        mask, reinterpret_cast<Npp16u*>(destination_planes[1]),
                        destination_steps[1], chroma_size, stream_context),
                    "量化P010色度平面");
  }
}

template <typename Sample>
void ConvertBgrToPlanar444(const cv::cuda::GpuMat& source,
                           MwStreamerVideoFrameView* destination,
                           const PixelFormatInfo& format,
                           const NppStreamContext& stream_context) {
  auto& linear = destination->buffer.storage.linear;
  if (linear.planes[0].stride_bytes != linear.planes[1].stride_bytes ||
      linear.planes[0].stride_bytes != linear.planes[2].stride_bytes) {
    throw std::invalid_argument(
        "CudaMatAdapter要求YUV444三个平面具有相同stride");
  }
  cv::cuda::GpuMat packed(static_cast<int>(destination->buffer.height),
                          static_cast<int>(destination->buffer.width),
                          sizeof(Sample) == 1 ? CV_8UC3 : CV_16UC3);
  const NppiSize size = {static_cast<int>(destination->buffer.width),
                         static_cast<int>(destination->buffer.height)};
  const auto matrix = MakeForwardMatrix(*destination, format);
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiColorTwist32f_8u_C3R_Ctx(
        source.ptr<Npp8u>(), static_cast<int>(source.step), packed.ptr<Npp8u>(),
        static_cast<int>(packed.step), size, MatrixData(matrix),
        stream_context);
  } else {
    status = nppiColorTwist32f_16u_C3R_Ctx(
        source.ptr<Npp16u>(), static_cast<int>(source.step),
        packed.ptr<Npp16u>(), static_cast<int>(packed.step), size,
        MatrixData(matrix), stream_context);
  }
  ThrowIfNppError(status, "NPP转换BGR到YUV444");

  Sample* destination_planes[3] = {
      reinterpret_cast<Sample*>(linear.planes[0].address),
      reinterpret_cast<Sample*>(linear.planes[1].address),
      reinterpret_cast<Sample*>(linear.planes[2].address)};
  if constexpr (sizeof(Sample) == 1) {
    status = nppiCopy_8u_C3P3R_Ctx(
        packed.ptr<Npp8u>(), static_cast<int>(packed.step), destination_planes,
        linear.planes[0].stride_bytes, size, stream_context);
  } else {
    status = nppiCopy_16u_C3P3R_Ctx(
        packed.ptr<Npp16u>(), static_cast<int>(packed.step), destination_planes,
        linear.planes[0].stride_bytes, size, stream_context);
  }
  ThrowIfNppError(status, "NPP拆分YUV444平面");
}

void ClampPlanar10(MwStreamerVideoFrameView* destination,
                   const PixelFormatInfo& format,
                   const NppStreamContext& stream_context) {
  auto& linear = destination->buffer.storage.linear;
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const auto& plane = linear.planes[index];
    const NppiSize plane_size = {
        static_cast<int>(plane.row_bytes / sizeof(Npp16u)),
        static_cast<int>(plane.row_count)};
    ThrowIfNppError(
        nppiThreshold_GTVal_16u_C1IR_Ctx(
            reinterpret_cast<Npp16u*>(plane.address), plane.stride_bytes,
            plane_size, static_cast<Npp16u>(MaxValue(format.value_bits)),
            static_cast<Npp16u>(MaxValue(format.value_bits)), stream_context),
        "限制planar 10-bit视频取值范围");
  }
}

template <typename Sample>
void ConvertBgrToPlanar420(const cv::cuda::GpuMat& source,
                           MwStreamerVideoFrameView* destination,
                           const PixelFormatInfo& format,
                           const NppStreamContext& stream_context) {
  auto& linear = destination->buffer.storage.linear;
  Sample* destination_planes[3] = {
      reinterpret_cast<Sample*>(linear.planes[0].address),
      reinterpret_cast<Sample*>(linear.planes[1].address),
      reinterpret_cast<Sample*>(linear.planes[2].address)};
  int destination_steps[3] = {linear.planes[0].stride_bytes,
                              linear.planes[1].stride_bytes,
                              linear.planes[2].stride_bytes};
  const NppiSize size = {static_cast<int>(destination->buffer.width),
                         static_cast<int>(destination->buffer.height)};
  const auto matrix = MakeForwardMatrix(*destination, format);
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiRGBToYUV420_8u_ColorTwist32f_C3P3R_Ctx(
        source.ptr<Npp8u>(), static_cast<int>(source.step), destination_planes,
        destination_steps, size, MatrixData(matrix), stream_context);
  } else {
    status = nppiRGBToYUV420_16u_ColorTwist32f_C3P3R_Ctx(
        source.ptr<Npp16u>(), static_cast<int>(source.step), destination_planes,
        destination_steps, size, MatrixData(matrix), stream_context);
  }
  ThrowIfNppError(status, "NPP转换BGR到planar YUV420");
  if (format.value_bits == 10) {
    ClampPlanar10(destination, format, stream_context);
  }
}

template <typename Sample>
void ConvertBgrToPlanar422(const cv::cuda::GpuMat& source,
                           MwStreamerVideoFrameView* destination,
                           const PixelFormatInfo& format,
                           const NppStreamContext& stream_context) {
  const int width = static_cast<int>(destination->buffer.width);
  const int height = static_cast<int>(destination->buffer.height);
  const int packed_type = sizeof(Sample) == 1 ? CV_8UC3 : CV_16UC3;
  const int plane_type = sizeof(Sample) == 1 ? CV_8UC1 : CV_16UC1;
  cv::cuda::GpuMat packed(height, width, packed_type);
  cv::cuda::GpuMat full_planes(height * 3, width, plane_type);
  const NppiSize full_size = {width, height};
  const auto matrix = MakeForwardMatrix(*destination, format);
  NppStatus status;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiColorTwist32f_8u_C3R_Ctx(
        source.ptr<Npp8u>(), static_cast<int>(source.step), packed.ptr<Npp8u>(),
        static_cast<int>(packed.step), full_size, MatrixData(matrix),
        stream_context);
  } else {
    status = nppiColorTwist32f_16u_C3R_Ctx(
        source.ptr<Npp16u>(), static_cast<int>(source.step),
        packed.ptr<Npp16u>(), static_cast<int>(packed.step), full_size,
        MatrixData(matrix), stream_context);
  }
  ThrowIfNppError(status, "NPP转换BGR到全分辨率YUV");

  Sample* temporary_planes[3] = {full_planes.ptr<Sample>(),
                                 full_planes.ptr<Sample>(height),
                                 full_planes.ptr<Sample>(height * 2)};
  if constexpr (sizeof(Sample) == 1) {
    status = nppiCopy_8u_C3P3R_Ctx(
        packed.ptr<Npp8u>(), static_cast<int>(packed.step), temporary_planes,
        static_cast<int>(full_planes.step), full_size, stream_context);
  } else {
    status = nppiCopy_16u_C3P3R_Ctx(
        packed.ptr<Npp16u>(), static_cast<int>(packed.step), temporary_planes,
        static_cast<int>(full_planes.step), full_size, stream_context);
  }
  ThrowIfNppError(status, "NPP拆分全分辨率YUV平面");

  auto& linear = destination->buffer.storage.linear;
  if constexpr (sizeof(Sample) == 1) {
    status = nppiCopy_8u_C1R_Ctx(
        temporary_planes[0], static_cast<int>(full_planes.step),
        reinterpret_cast<Npp8u*>(linear.planes[0].address),
        linear.planes[0].stride_bytes, full_size, stream_context);
  } else {
    status = nppiCopy_16u_C1R_Ctx(
        temporary_planes[0], static_cast<int>(full_planes.step),
        reinterpret_cast<Npp16u*>(linear.planes[0].address),
        linear.planes[0].stride_bytes, full_size, stream_context);
  }
  ThrowIfNppError(status, "NPP复制YUV422亮度平面");

  const NppiSize chroma_size = {width / 2, height};
  const NppiRect source_roi = {0, 0, width, height};
  const NppiRect destination_roi = {0, 0, width / 2, height};
  for (int index = 1; index < 3; ++index) {
    if constexpr (sizeof(Sample) == 1) {
      status = nppiResize_8u_C1R_Ctx(
          temporary_planes[index], static_cast<int>(full_planes.step),
          full_size, source_roi,
          reinterpret_cast<Npp8u*>(linear.planes[index].address),
          linear.planes[index].stride_bytes, chroma_size, destination_roi,
          NPPI_INTER_LINEAR, stream_context);
    } else {
      status = nppiResize_16u_C1R_Ctx(
          temporary_planes[index], static_cast<int>(full_planes.step),
          full_size, source_roi,
          reinterpret_cast<Npp16u*>(linear.planes[index].address),
          linear.planes[index].stride_bytes, chroma_size, destination_roi,
          NPPI_INTER_LINEAR, stream_context);
    }
    ThrowIfNppError(status, "NPP重采样YUV422色度平面");
  }
  if (format.value_bits == 10) {
    ClampPlanar10(destination, format, stream_context);
  }
}

}  // namespace

cv::cuda::GpuMat CudaMatAdapter::ToBgr(const MwStreamerVideoFrameView& source) {
  const auto format = ValidatePrototype(source);
  auto cuda_source = CudaFrame::CopyFrom(source);
  const auto& cuda_view = cuda_source.view();
  for (std::uint32_t index = 0;
       index < cuda_view.buffer.storage.linear.plane_count; ++index) {
    ValidateNppStep(cuda_view.buffer.storage.linear.planes[index].stride_bytes);
  }

  cv::cuda::GpuMat destination(static_cast<int>(source.buffer.height),
                               static_cast<int>(source.buffer.width),
                               format.mat_type);
  if (destination.empty() ||
      destination.step >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("分配OpenCV CUDA BGR GpuMat失败");
  }
  const auto stream_context = MakeNppStreamContext();
  if (IsSemiPlanar(format)) {
    if (format.bytes_per_sample == 1) {
      ConvertSemiPlanarToBgr<Npp8u>(cuda_view, &destination, format,
                                    stream_context);
    } else {
      ConvertSemiPlanarToBgr<Npp16u>(cuda_view, &destination, format,
                                     stream_context);
    }
  } else if (format.layout == PixelLayout::kPlanar444) {
    if (format.bytes_per_sample == 1) {
      ConvertPlanar444ToBgr<Npp8u>(cuda_view, &destination, format,
                                   stream_context);
    } else {
      ConvertPlanar444ToBgr<Npp16u>(cuda_view, &destination, format,
                                    stream_context);
    }
  } else if (format.bytes_per_sample == 1) {
    ConvertSubsampledPlanarToBgr<Npp8u>(cuda_view, &destination, format,
                                        stream_context);
  } else {
    ConvertSubsampledPlanarToBgr<Npp16u>(cuda_view, &destination, format,
                                         stream_context);
  }
  ThrowIfCudaError(cudaDeviceSynchronize(), "等待CUDA BGR转换完成");
  return destination;
}

CudaFrame CudaMatAdapter::FromBgr(const cv::cuda::GpuMat& source,
                                  const MwStreamerVideoFrameView& prototype) {
  const auto format = ValidatePrototype(prototype);
  ValidateGpuMat(source, prototype, format);
  auto destination = CudaFrame::AllocateLike(prototype);
  auto& destination_view = destination.mutable_view();
  const auto stream_context = MakeNppStreamContext();
  if (IsSemiPlanar(format)) {
    if (format.bytes_per_sample == 1) {
      ConvertBgrToSemiPlanar<Npp8u>(source, &destination_view, format,
                                    stream_context);
    } else {
      ConvertBgrToSemiPlanar<Npp16u>(source, &destination_view, format,
                                     stream_context);
    }
  } else if (format.layout == PixelLayout::kPlanar444) {
    if (format.bytes_per_sample == 1) {
      ConvertBgrToPlanar444<Npp8u>(source, &destination_view, format,
                                   stream_context);
    } else {
      ConvertBgrToPlanar444<Npp16u>(source, &destination_view, format,
                                    stream_context);
      if (format.value_bits == 10) {
        ClampPlanar10(&destination_view, format, stream_context);
      }
    }
  } else if (format.layout == PixelLayout::kPlanar420) {
    if (format.bytes_per_sample == 1) {
      ConvertBgrToPlanar420<Npp8u>(source, &destination_view, format,
                                   stream_context);
    } else {
      ConvertBgrToPlanar420<Npp16u>(source, &destination_view, format,
                                    stream_context);
    }
  } else if (format.bytes_per_sample == 1) {
    ConvertBgrToPlanar422<Npp8u>(source, &destination_view, format,
                                 stream_context);
  } else {
    ConvertBgrToPlanar422<Npp16u>(source, &destination_view, format,
                                  stream_context);
  }
  ThrowIfCudaError(cudaDeviceSynchronize(), "等待CUDA YUV转换完成");
  return destination;
}

}  // namespace mw::streamer::opencv_adapter

#include "mw/opencv_adapter/host_mat_adapter.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace mw::streamer::opencv_adapter {
namespace {

struct PixelFormatInfo {
  AVPixelFormat yuv_format = AV_PIX_FMT_NONE;
  AVPixelFormat bgr_format = AV_PIX_FMT_NONE;
  int mat_type = -1;
  std::uint32_t bytes_per_sample = 0;
};

using SwsContextPtr = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;

PixelFormatInfo GetPixelFormatInfo(MwStreamerVideoPixelFormat pixel_format) {
  switch (pixel_format) {
    case kMwStreamerVideoPixelFormatNv12:
      return {AV_PIX_FMT_NV12, AV_PIX_FMT_BGR24, CV_8UC3, 1};
    case kMwStreamerVideoPixelFormatP010:
      return {AV_PIX_FMT_P010LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    case kMwStreamerVideoPixelFormatP016:
      return {AV_PIX_FMT_P016LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    case kMwStreamerVideoPixelFormatYuv444p:
      return {AV_PIX_FMT_YUV444P, AV_PIX_FMT_BGR24, CV_8UC3, 1};
    case kMwStreamerVideoPixelFormatYuv444p16le:
      return {AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    case kMwStreamerVideoPixelFormatYuv420p:
      return {AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGR24, CV_8UC3, 1};
    case kMwStreamerVideoPixelFormatYuv422p:
      return {AV_PIX_FMT_YUV422P, AV_PIX_FMT_BGR24, CV_8UC3, 1};
    case kMwStreamerVideoPixelFormatYuv420p10le:
      return {AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    case kMwStreamerVideoPixelFormatYuv422p10le:
      return {AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    case kMwStreamerVideoPixelFormatYuv444p10le:
      return {AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_BGR48LE, CV_16UC3, 2};
    default:
      throw std::invalid_argument("HostMatAdapter不支持该视频像素格式");
  }
}

int GetSwsColorspace(MwStreamerColorSpace colorspace) {
  switch (colorspace) {
    case kMwStreamerColorSpaceBt709:
      return SWS_CS_ITU709;
    case kMwStreamerColorSpaceFcc:
      return SWS_CS_FCC;
    case kMwStreamerColorSpaceBt470bg:
    case kMwStreamerColorSpaceSmpte170m:
      return SWS_CS_ITU601;
    case kMwStreamerColorSpaceSmpte240m:
      return SWS_CS_SMPTE240M;
    case kMwStreamerColorSpaceBt2020Ncl:
      return SWS_CS_BT2020;
    default:
      throw std::invalid_argument("HostMatAdapter不支持该视频颜色空间");
  }
}

int GetRange(MwStreamerColorRange range) {
  switch (range) {
    case kMwStreamerColorRangeLimited:
      return 0;
    case kMwStreamerColorRangeFull:
      return 1;
    default:
      throw std::invalid_argument("HostMatAdapter不支持未知视频颜色范围");
  }
}

void ValidateTransfer(MwStreamerColorTransfer transfer) {
  if (transfer == kMwStreamerColorTransferUnknown) {
    throw std::invalid_argument("HostMatAdapter不支持未知视频传递函数");
  }
  if (transfer == kMwStreamerColorTransferSmpte2084 ||
      transfer == kMwStreamerColorTransferAribStdB67) {
    throw std::invalid_argument("HostMatAdapter暂不支持PQ或HLG视频");
  }
}

PixelFormatInfo ValidatePrototype(const MwStreamerVideoFrameView& prototype) {
  const auto& buffer = prototype.buffer;
  const auto format = GetPixelFormatInfo(buffer.pixel_format);
  if (buffer.memory_type != kMwStreamerMemoryHost &&
      buffer.memory_type != kMwStreamerMemoryCuda) {
    throw std::invalid_argument("HostMatAdapter收到未知视频内存类型");
  }
  if (buffer.storage_type != kMwStreamerVideoStorageLinear) {
    throw std::invalid_argument("HostMatAdapter只接受linear视频存储");
  }
  if (buffer.width == 0 || buffer.height == 0 ||
      buffer.width >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) /
              format.bytes_per_sample ||
      buffer.height >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("HostMatAdapter要求有效的视频宽高");
  }

  const auto* descriptor = av_pix_fmt_desc_get(format.yuv_format);
  const int expected_plane_count = av_pix_fmt_count_planes(format.yuv_format);
  std::array<int, 4> expected_row_bytes{};
  if (!descriptor || expected_plane_count <= 0 || expected_plane_count > 4 ||
      av_image_fill_linesizes(expected_row_bytes.data(), format.yuv_format,
                              static_cast<int>(buffer.width)) < 0) {
    throw std::runtime_error("HostMatAdapter无法计算视频平面布局");
  }

  const auto& linear = buffer.storage.linear;
  if (!linear.planes ||
      linear.plane_count != static_cast<std::uint32_t>(expected_plane_count)) {
    throw std::invalid_argument("HostMatAdapter收到错误的视频平面数量");
  }

  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    const std::uint32_t expected_row_count =
        index == 0 || descriptor->log2_chroma_h == 0
            ? buffer.height
            : (buffer.height + (1U << descriptor->log2_chroma_h) - 1) >>
                  descriptor->log2_chroma_h;
    const auto row_bytes =
        static_cast<std::uint32_t>(expected_row_bytes[index]);
    const auto stride =
        static_cast<std::int64_t>(linear.planes[index].stride_bytes);
    const auto absolute_stride = stride < 0 ? -stride : stride;
    if (linear.planes[index].address == 0 ||
        linear.planes[index].row_bytes != row_bytes ||
        linear.planes[index].row_count != expected_row_count ||
        absolute_stride < row_bytes) {
      throw std::invalid_argument("HostMatAdapter收到无效的视频平面布局");
    }
  }

  GetSwsColorspace(prototype.color.space);
  GetRange(prototype.color.range);
  ValidateTransfer(prototype.color.transfer);
  return format;
}

SwsContextPtr MakeContext(const MwStreamerVideoFrameView& prototype,
                          AVPixelFormat source_format,
                          AVPixelFormat destination_format, int source_range,
                          int destination_range) {
  const int width = static_cast<int>(prototype.buffer.width);
  const int height = static_cast<int>(prototype.buffer.height);
  SwsContextPtr context(
      sws_getContext(width, height, source_format, width, height,
                     destination_format, SWS_BILINEAR | SWS_ACCURATE_RND,
                     nullptr, nullptr, nullptr),
      &sws_freeContext);
  if (!context) {
    throw std::runtime_error("创建libswscale转换上下文失败");
  }

  const int colorspace = GetSwsColorspace(prototype.color.space);
  const int* coefficients = sws_getCoefficients(colorspace);
  if (sws_setColorspaceDetails(context.get(), coefficients, source_range,
                               coefficients, destination_range, 0, 1 << 16,
                               1 << 16) < 0) {
    throw std::runtime_error("设置libswscale颜色参数失败");
  }
  return context;
}

void CheckConvertedRows(int converted_rows, std::uint32_t height) {
  if (converted_rows != static_cast<int>(height)) {
    throw std::runtime_error("libswscale未生成完整视频帧");
  }
}

}  // namespace

cv::Mat HostMatAdapter::ToBgr(const MwStreamerVideoFrameView& source) {
  const auto format = ValidatePrototype(source);
  if (source.buffer.memory_type == kMwStreamerMemoryCuda) {
    const auto host_source = HostFrame::CopyFrom(source);
    return ToBgr(host_source.view());
  }

  const int width = static_cast<int>(source.buffer.width);
  const int height = static_cast<int>(source.buffer.height);
  cv::Mat destination(height, width, format.mat_type);
  if (destination.empty() ||
      destination.step >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("分配OpenCV BGR Mat失败");
  }

  const auto& linear = source.buffer.storage.linear;
  std::array<const std::uint8_t*, 4> source_data{};
  std::array<int, 4> source_stride{};
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    source_data[index] =
        reinterpret_cast<const std::uint8_t*>(linear.planes[index].address);
    source_stride[index] = linear.planes[index].stride_bytes;
  }
  std::array<std::uint8_t*, 4> destination_data = {destination.data, nullptr,
                                                   nullptr, nullptr};
  const std::array<int, 4> destination_stride = {
      static_cast<int>(destination.step), 0, 0, 0};
  auto context = MakeContext(source, format.yuv_format, format.bgr_format,
                             GetRange(source.color.range), 1);
  CheckConvertedRows(
      sws_scale(context.get(), source_data.data(), source_stride.data(), 0,
                height, destination_data.data(), destination_stride.data()),
      source.buffer.height);
  return destination;
}

HostFrame HostMatAdapter::FromBgr(const cv::Mat& source,
                                  const MwStreamerVideoFrameView& prototype) {
  const auto format = ValidatePrototype(prototype);
  if (source.empty() || source.dims != 2 || source.cols < 0 ||
      source.rows < 0 ||
      static_cast<std::uint32_t>(source.cols) != prototype.buffer.width ||
      static_cast<std::uint32_t>(source.rows) != prototype.buffer.height ||
      source.type() != format.mat_type ||
      source.step > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("OpenCV BGR Mat与视频原型不匹配");
  }

  auto destination = HostFrame::AllocateLike(prototype);
  auto& destination_view = destination.mutable_view();
  auto& linear = destination_view.buffer.storage.linear;
  const std::array<const std::uint8_t*, 4> source_data = {source.data, nullptr,
                                                          nullptr, nullptr};
  const std::array<int, 4> source_stride = {static_cast<int>(source.step), 0, 0,
                                            0};
  std::array<std::uint8_t*, 4> destination_data{};
  std::array<int, 4> destination_stride{};
  for (std::uint32_t index = 0; index < linear.plane_count; ++index) {
    destination_data[index] =
        reinterpret_cast<std::uint8_t*>(linear.planes[index].address);
    destination_stride[index] = linear.planes[index].stride_bytes;
  }
  auto context = MakeContext(prototype, format.bgr_format, format.yuv_format, 1,
                             GetRange(prototype.color.range));
  CheckConvertedRows(
      sws_scale(context.get(), source_data.data(), source_stride.data(), 0,
                source.rows, destination_data.data(),
                destination_stride.data()),
      prototype.buffer.height);
  return destination;
}

}  // namespace mw::streamer::opencv_adapter

#include "mw/frame_convter/opencv_cpu_convter.h"

#include <climits>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace mw::frame_convter {
namespace {

namespace ffmpeg = streamer::ffmpeg;

const AVPixFmtDescriptor& ValidateCpuFormat(
    const streamer::ffmpeg::Frame& frame) {
  if (!frame.get()) throw std::invalid_argument("不能转换已移动的帧");
  const auto* description =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
  if (!description || (description->flags & AV_PIX_FMT_FLAG_HWACCEL) ||
      frame->hw_frames_ctx) {
    throw std::invalid_argument("需要有效的 CPU 像素格式");
  }
  return *description;
}

void ValidateCpuPixels(const streamer::ffmpeg::Frame& frame) {
  const auto& description = ValidateCpuFormat(frame);
  if (frame->width <= 0 || frame->height <= 0 ||
      av_image_check_size(static_cast<unsigned int>(frame->width),
                          static_cast<unsigned int>(frame->height), 0,
                          nullptr) < 0) {
    throw std::invalid_argument("CPU 帧尺寸无效");
  }
  int minimum_linesizes[4] = {};
  if (av_image_fill_linesizes(minimum_linesizes,
                              static_cast<AVPixelFormat>(frame->format),
                              frame->width) < 0) {
    throw std::invalid_argument("无法确定 CPU 帧平面布局");
  }
  for (int plane = 0; plane < 4; ++plane) {
    if (minimum_linesizes[plane] == 0) continue;
    const auto stride = static_cast<std::int64_t>(frame->linesize[plane]);
    if (!frame->data[plane] ||
        (stride < 0 ? -stride : stride) < minimum_linesizes[plane]) {
      throw std::invalid_argument("CPU 帧数据或平面步长无效");
    }
  }
  if ((description.flags & AV_PIX_FMT_FLAG_PAL) && !frame->data[1]) {
    throw std::invalid_argument("CPU 调色板帧缺少调色板");
  }
}

AVPixelFormat RgbFormat(RgbOrder order) {
  switch (order) {
    case RgbOrder::kRgb:
      return AV_PIX_FMT_RGB24;
    case RgbOrder::kBgr:
      return AV_PIX_FMT_BGR24;
  }
  throw std::invalid_argument("RGB 通道顺序无效");
}

bool IsRgb(const AVFrame& frame) {
  const auto* description =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
  return description && (description->flags & AV_PIX_FMT_FLAG_RGB);
}

int Matrix(const AVFrame& frame) {
  switch (frame.colorspace) {
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_BT709:
      return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      return SWS_CS_ITU601;
    case AVCOL_SPC_FCC:
      return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M:
      return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
      return SWS_CS_BT2020;
    default:
      throw std::invalid_argument("请在 AVFrame 中指定受支持的 YUV colorspace");
  }
}

int Range(const AVFrame& frame) {
  if (IsRgb(frame)) {
    if (frame.color_range == AVCOL_RANGE_MPEG) {
      throw std::invalid_argument("CPU RGB 转换仅支持全范围 RGB");
    }
    return 1;
  }
  switch (frame.color_range) {
    case AVCOL_RANGE_MPEG:
      return 0;
    case AVCOL_RANGE_JPEG:
      return 1;
    default:
      throw std::invalid_argument("请在 AVFrame 中指定 YUV color_range");
  }
}

void CheckColorEncoding(const AVFrame& source, const AVFrame& target) {
  if ((source.color_primaries != AVCOL_PRI_UNSPECIFIED &&
       target.color_primaries != AVCOL_PRI_UNSPECIFIED &&
       source.color_primaries != target.color_primaries) ||
      (source.color_trc != AVCOL_TRC_UNSPECIFIED &&
       target.color_trc != AVCOL_TRC_UNSPECIFIED &&
       source.color_trc != target.color_trc)) {
    throw std::invalid_argument("CPU 帧转换不支持改变色域或传递函数");
  }
  if (IsRgb(source) != IsRgb(target)) {
    const auto trc = IsRgb(source) ? target.color_trc : source.color_trc;
    if (trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67) {
      throw std::invalid_argument("HDR 与 RGB 的转换需要独立的颜色管理");
    }
  }
}

void SetChromaPosition(SwsContext* context, const AVFrame& frame, bool source) {
  if (IsRgb(frame) || frame.chroma_location == AVCHROMA_LOC_UNSPECIFIED) return;
  int x = 0;
  int y = 0;
  switch (frame.chroma_location) {
    case AVCHROMA_LOC_LEFT:
      y = 128;
      break;
    case AVCHROMA_LOC_CENTER:
      x = y = 128;
      break;
    case AVCHROMA_LOC_TOPLEFT:
      break;
    case AVCHROMA_LOC_TOP:
      x = 128;
      break;
    case AVCHROMA_LOC_BOTTOMLEFT:
      y = 256;
      break;
    case AVCHROMA_LOC_BOTTOM:
      x = 128;
      y = 256;
      break;
    default:
      throw std::invalid_argument("AVFrame chroma_location 无效");
  }
  ffmpeg::FfmpegException::throwIfError(
      av_opt_set_int(context, source ? "src_h_chr_pos" : "dst_h_chr_pos", x, 0),
      "设置水平色度采样位置");
  ffmpeg::FfmpegException::throwIfError(
      av_opt_set_int(context, source ? "src_v_chr_pos" : "dst_v_chr_pos", y, 0),
      "设置垂直色度采样位置");
}

void ConvertPixels(const AVFrame& source, AVFrame& target) {
  const auto source_format = static_cast<AVPixelFormat>(source.format);
  const auto target_format = static_cast<AVPixelFormat>(target.format);
  if (!sws_isSupportedInput(source_format) ||
      !sws_isSupportedOutput(target_format)) {
    throw std::invalid_argument("libswscale 不支持指定的 CPU 像素格式");
  }
  CheckColorEncoding(source, target);
  if (source.format == target.format &&
      source.colorspace == target.colorspace &&
      source.color_range == target.color_range &&
      source.chroma_location == target.chroma_location) {
    ffmpeg::FfmpegException::throwIfError(av_frame_copy(&target, &source),
                                          "复制 CPU 像素");
    return;
  }
  const bool source_rgb = IsRgb(source);
  const bool target_rgb = IsRgb(target);
  const int source_matrix = source_rgb
                                ? (target_rgb ? SWS_CS_ITU709 : Matrix(target))
                                : Matrix(source);
  const int target_matrix = target_rgb ? source_matrix : Matrix(target);
  const int source_range = Range(source);
  const int target_range = Range(target);
  const std::unique_ptr<SwsContext, decltype(&sws_freeContext)> context(
      sws_alloc_context(), sws_freeContext);
  if (!context) throw std::bad_alloc();
  const auto set = [&](const char* key, std::int64_t value) {
    ffmpeg::FfmpegException::throwIfError(
        av_opt_set_int(context.get(), key, value, 0), "设置 CPU 转换参数");
  };
  set("srcw", source.width);
  set("srch", source.height);
  set("src_format", source_format);
  set("dstw", target.width);
  set("dsth", target.height);
  set("dst_format", target_format);
  set("sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND);
  SetChromaPosition(context.get(), source, true);
  SetChromaPosition(context.get(), target, false);
  // Unscaled shortcuts can ignore explicit chroma positions. An identity
  // filter selects the general scaler without changing the sampling kernel.
  double identity_coefficients[] = {0.0, 1.0, 0.0};
  SwsVector identity{identity_coefficients, 3};
  SwsFilter filter{};
  filter.chrH = &identity;
  filter.chrV = &identity;
  const bool explicit_chroma =
      (!source_rgb && source.chroma_location != AVCHROMA_LOC_UNSPECIFIED) ||
      (!target_rgb && target.chroma_location != AVCHROMA_LOC_UNSPECIFIED);
  ffmpeg::FfmpegException::throwIfError(
      sws_init_context(context.get(), explicit_chroma ? &filter : nullptr,
                       nullptr),
      "初始化 CPU 转换参数");
  ffmpeg::FfmpegException::throwIfError(
      sws_setColorspaceDetails(context.get(),
                               sws_getCoefficients(source_matrix), source_range,
                               sws_getCoefficients(target_matrix), target_range,
                               0, 1 << 16, 1 << 16),
      "设置 CPU 帧颜色信息");
  const int rows = sws_scale(context.get(), source.data, source.linesize, 0,
                             source.height, target.data, target.linesize);
  ffmpeg::FfmpegException::throwIfError(rows, "转换 CPU 帧像素");
  if (rows != target.height) {
    throw std::runtime_error("CPU 帧颜色转换未生成完整图像");
  }
}

ffmpeg::Frame OutputFrame(const ffmpeg::Frame& reference, int width,
                          int height) {
  ValidateCpuFormat(reference);
  ffmpeg::Frame output;
  output.CopyPropertiesFrom(reference);
  output->format = reference->format;
  output->width = width;
  output->height = height;
  output.ClearCrop();
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(output.get(), 32),
                                        "分配 CPU 转换帧");
  return output;
}

}  // namespace

cv::Mat OpencvCpuConvter::ToMat(const ffmpeg::Frame& frame, RgbOrder order) {
  ValidateCpuPixels(frame);
  const auto format = RgbFormat(order);
  cv::Mat output(frame->height, frame->width, CV_8UC3);
  if (output.step[0] > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument("OpenCV 图像步长超过 FFmpeg 限制");
  }
  ffmpeg::Frame target;
  target.CopyPropertiesFrom(frame);
  target->format = format;
  target->width = output.cols;
  target->height = output.rows;
  target->colorspace = AVCOL_SPC_RGB;
  target->color_range = AVCOL_RANGE_JPEG;
  target->data[0] = output.data;
  target->linesize[0] = static_cast<int>(output.step[0]);
  ConvertPixels(*frame.get(), *target.get());
  return output;
}

ffmpeg::Frame OpencvCpuConvter::ToFrame(const cv::Mat& image,
                                        const ffmpeg::Frame& reference,
                                        RgbOrder order) {
  if (image.empty() || image.dims != 2 || image.type() != CV_8UC3 ||
      image.step[0] > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument("需要有效的二维 CV_8UC3 图像");
  }
  auto output = OutputFrame(reference, image.cols, image.rows);
  ffmpeg::Frame source;
  source.CopyPropertiesFrom(reference);
  source->format = RgbFormat(order);
  source->width = image.cols;
  source->height = image.rows;
  source->colorspace = AVCOL_SPC_RGB;
  source->color_range = AVCOL_RANGE_JPEG;
  source->data[0] = image.data;
  source->linesize[0] = static_cast<int>(image.step[0]);
  ConvertPixels(*source.get(), *output.get());
  return output;
}

ffmpeg::Frame OpencvCpuConvter::Convert(const ffmpeg::Frame& source,
                                        const ffmpeg::Frame& reference) {
  ValidateCpuPixels(source);
  auto output = OutputFrame(reference, source->width, source->height);
  ConvertPixels(*source.get(), *output.get());
  return output;
}

}  // namespace mw::frame_convter

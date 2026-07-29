#include "mw/processor/frame_adapter.h"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/error.h"

namespace mw::streamer::processor {
namespace {

constexpr int kProcessorAudioSampleRate = 48000;

const char* PixelFormatName(AVPixelFormat format) {
  const char* name = av_get_pix_fmt_name(format);
  return name ? name : "unknown";
}

MwStreamerVideoPixelFormat MapPixelFormat(AVPixelFormat format) {
  switch (format) {
    case AV_PIX_FMT_NV12:
      return kMwStreamerVideoPixelFormatNv12;
    case AV_PIX_FMT_P010LE:
      return kMwStreamerVideoPixelFormatP010;
    case AV_PIX_FMT_YUV420P:
      return kMwStreamerVideoPixelFormatYuv420p;
    case AV_PIX_FMT_YUV422P:
      return kMwStreamerVideoPixelFormatYuv422p;
    case AV_PIX_FMT_YUV444P:
      return kMwStreamerVideoPixelFormatYuv444p;
    case AV_PIX_FMT_YUV420P10LE:
      return kMwStreamerVideoPixelFormatYuv420p10le;
    case AV_PIX_FMT_YUV422P10LE:
      return kMwStreamerVideoPixelFormatYuv422p10le;
    case AV_PIX_FMT_YUV444P10LE:
      return kMwStreamerVideoPixelFormatYuv444p10le;
    default:
      throw std::invalid_argument(fmt::format("Processor不支持视频像素格式: {}",
                                              PixelFormatName(format)));
  }
}

std::uint32_t PlaneRowCount(const AVPixFmtDescriptor& descriptor,
                            std::uint32_t plane, std::uint32_t height) {
  if (plane == 0 || descriptor.log2_chroma_h == 0) {
    return height;
  }
  const std::uint32_t alignment = 1U << descriptor.log2_chroma_h;
  return (height + alignment - 1) >> descriptor.log2_chroma_h;
}

void MapVideoBuffer(const AVFrame& frame,
                    std::array<MwStreamerVideoPlaneView, 4>* mapped_planes,
                    MwStreamerVideoBufferView* view) {
  if (frame.width <= 0 || frame.height <= 0) {
    throw std::invalid_argument("视频帧缺少有效的宽高");
  }

  auto memory_type = kMwStreamerMemoryHost;
  auto storage_format = static_cast<AVPixelFormat>(frame.format);
  if (storage_format == AV_PIX_FMT_CUDA) {
    if (!frame.hw_frames_ctx || !frame.hw_frames_ctx->data) {
      throw std::invalid_argument("CUDA视频帧缺少硬件帧上下文");
    }
    const auto* frames_context =
        reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
    if (frames_context->format != AV_PIX_FMT_CUDA ||
        !frames_context->device_ctx ||
        frames_context->device_ctx->type != AV_HWDEVICE_TYPE_CUDA) {
      throw std::invalid_argument("视频帧不是有效的CUDA硬件帧");
    }
    memory_type = kMwStreamerMemoryCuda;
    storage_format = frames_context->sw_format;
  }

  const auto* descriptor = av_pix_fmt_desc_get(storage_format);
  if (!descriptor) {
    throw std::invalid_argument("视频帧包含无效的存储格式");
  }
  if (memory_type == kMwStreamerMemoryHost &&
      (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
    throw std::invalid_argument(
        fmt::format("Processor暂不支持硬件视频帧格式: {}",
                    PixelFormatName(storage_format)));
  }

  const auto pixel_format = MapPixelFormat(storage_format);
  const int plane_count = av_pix_fmt_count_planes(storage_format);
  if (plane_count <= 0 || plane_count > 4) {
    throw std::invalid_argument(
        fmt::format("视频像素格式具有无效的平面数量: format={}, planes={}",
                    PixelFormatName(storage_format), plane_count));
  }
  const auto mapped_plane_count = static_cast<std::uint32_t>(plane_count);

  std::array<int, 4> row_bytes{};
  ffmpeg::ThrowIfError(
      av_image_fill_linesizes(row_bytes.data(), storage_format, frame.width),
      "计算视频平面有效行宽");

  for (std::uint32_t plane = 0; plane < mapped_plane_count; ++plane) {
    if (!frame.data[plane] || frame.linesize[plane] == 0 ||
        std::abs(static_cast<std::int64_t>(frame.linesize[plane])) <
            row_bytes[plane]) {
      throw std::invalid_argument(
          fmt::format("视频帧平面无效: format={}, plane={}",
                      PixelFormatName(storage_format), plane));
    }
    (*mapped_planes)[plane] = {
        reinterpret_cast<std::uintptr_t>(frame.data[plane]),
        frame.linesize[plane],
        static_cast<std::uint32_t>(row_bytes[plane]),
        PlaneRowCount(*descriptor, plane,
                      static_cast<std::uint32_t>(frame.height)),
    };
  }

  *view = {};
  view->memory_type = memory_type;
  view->storage_type = kMwStreamerVideoStorageLinear;
  view->pixel_format = pixel_format;
  view->width = static_cast<std::uint32_t>(frame.width);
  view->height = static_cast<std::uint32_t>(frame.height);
  view->storage.linear = {
      mapped_planes->data(),
      mapped_plane_count,
  };
}

MwStreamerMediaTimestamp MapTimestamp(const AVFrame& frame) {
  if (frame.time_base.num <= 0 || frame.time_base.den <= 0) {
    throw std::invalid_argument("AVFrame缺少有效的time_base");
  }
  return {
      frame.pts,
      frame.duration,
      {
          frame.time_base.num,
          frame.time_base.den,
      },
  };
}

MwStreamerColorRange MapColorRange(AVColorRange range) {
  switch (range) {
    case AVCOL_RANGE_MPEG:
      return kMwStreamerColorRangeLimited;
    case AVCOL_RANGE_JPEG:
      return kMwStreamerColorRangeFull;
    default:
      return kMwStreamerColorRangeUnknown;
  }
}

MwStreamerColorSpace MapColorSpace(AVColorSpace space) {
  switch (space) {
    case AVCOL_SPC_RGB:
      return kMwStreamerColorSpaceRgb;
    case AVCOL_SPC_BT709:
      return kMwStreamerColorSpaceBt709;
    case AVCOL_SPC_FCC:
      return kMwStreamerColorSpaceFcc;
    case AVCOL_SPC_BT470BG:
      return kMwStreamerColorSpaceBt470bg;
    case AVCOL_SPC_SMPTE170M:
      return kMwStreamerColorSpaceSmpte170m;
    case AVCOL_SPC_SMPTE240M:
      return kMwStreamerColorSpaceSmpte240m;
    case AVCOL_SPC_YCGCO:
      return kMwStreamerColorSpaceYcgco;
    case AVCOL_SPC_BT2020_NCL:
      return kMwStreamerColorSpaceBt2020Ncl;
    case AVCOL_SPC_BT2020_CL:
      return kMwStreamerColorSpaceBt2020Cl;
    case AVCOL_SPC_SMPTE2085:
      return kMwStreamerColorSpaceSmpte2085;
    case AVCOL_SPC_CHROMA_DERIVED_NCL:
      return kMwStreamerColorSpaceChromaDerivedNcl;
    case AVCOL_SPC_CHROMA_DERIVED_CL:
      return kMwStreamerColorSpaceChromaDerivedCl;
    case AVCOL_SPC_ICTCP:
      return kMwStreamerColorSpaceIctcp;
    case AVCOL_SPC_IPT_C2:
      return kMwStreamerColorSpaceIptC2;
    case AVCOL_SPC_YCGCO_RE:
      return kMwStreamerColorSpaceYcgcoRe;
    case AVCOL_SPC_YCGCO_RO:
      return kMwStreamerColorSpaceYcgcoRo;
    default:
      return kMwStreamerColorSpaceUnknown;
  }
}

MwStreamerColorPrimaries MapColorPrimaries(AVColorPrimaries primaries) {
  switch (primaries) {
    case AVCOL_PRI_BT709:
      return kMwStreamerColorPrimariesBt709;
    case AVCOL_PRI_BT470M:
      return kMwStreamerColorPrimariesBt470m;
    case AVCOL_PRI_BT470BG:
      return kMwStreamerColorPrimariesBt470bg;
    case AVCOL_PRI_SMPTE170M:
      return kMwStreamerColorPrimariesSmpte170m;
    case AVCOL_PRI_SMPTE240M:
      return kMwStreamerColorPrimariesSmpte240m;
    case AVCOL_PRI_FILM:
      return kMwStreamerColorPrimariesFilm;
    case AVCOL_PRI_BT2020:
      return kMwStreamerColorPrimariesBt2020;
    case AVCOL_PRI_SMPTE428:
      return kMwStreamerColorPrimariesSmpte428;
    case AVCOL_PRI_SMPTE431:
      return kMwStreamerColorPrimariesSmpte431;
    case AVCOL_PRI_SMPTE432:
      return kMwStreamerColorPrimariesSmpte432;
    case AVCOL_PRI_EBU3213:
      return kMwStreamerColorPrimariesEbu3213;
    default:
      return kMwStreamerColorPrimariesUnknown;
  }
}

MwStreamerColorTransfer MapColorTransfer(
    AVColorTransferCharacteristic transfer) {
  switch (transfer) {
    case AVCOL_TRC_BT709:
      return kMwStreamerColorTransferBt709;
    case AVCOL_TRC_GAMMA22:
      return kMwStreamerColorTransferGamma22;
    case AVCOL_TRC_GAMMA28:
      return kMwStreamerColorTransferGamma28;
    case AVCOL_TRC_SMPTE170M:
      return kMwStreamerColorTransferSmpte170m;
    case AVCOL_TRC_SMPTE240M:
      return kMwStreamerColorTransferSmpte240m;
    case AVCOL_TRC_LINEAR:
      return kMwStreamerColorTransferLinear;
    case AVCOL_TRC_LOG:
      return kMwStreamerColorTransferLog;
    case AVCOL_TRC_LOG_SQRT:
      return kMwStreamerColorTransferLogSqrt;
    case AVCOL_TRC_IEC61966_2_4:
      return kMwStreamerColorTransferIec61966_2_4;
    case AVCOL_TRC_BT1361_ECG:
      return kMwStreamerColorTransferBt1361Ecg;
    case AVCOL_TRC_IEC61966_2_1:
      return kMwStreamerColorTransferIec61966_2_1;
    case AVCOL_TRC_BT2020_10:
      return kMwStreamerColorTransferBt2020_10;
    case AVCOL_TRC_BT2020_12:
      return kMwStreamerColorTransferBt2020_12;
    case AVCOL_TRC_SMPTE2084:
      return kMwStreamerColorTransferSmpte2084;
    case AVCOL_TRC_SMPTE428:
      return kMwStreamerColorTransferSmpte428;
    case AVCOL_TRC_ARIB_STD_B67:
      return kMwStreamerColorTransferAribStdB67;
    default:
      return kMwStreamerColorTransferUnknown;
  }
}

MwStreamerChromaLocation MapChromaLocation(AVChromaLocation location) {
  switch (location) {
    case AVCHROMA_LOC_LEFT:
      return kMwStreamerChromaLocationLeft;
    case AVCHROMA_LOC_CENTER:
      return kMwStreamerChromaLocationCenter;
    case AVCHROMA_LOC_TOPLEFT:
      return kMwStreamerChromaLocationTopLeft;
    case AVCHROMA_LOC_TOP:
      return kMwStreamerChromaLocationTop;
    case AVCHROMA_LOC_BOTTOMLEFT:
      return kMwStreamerChromaLocationBottomLeft;
    case AVCHROMA_LOC_BOTTOM:
      return kMwStreamerChromaLocationBottom;
    default:
      return kMwStreamerChromaLocationUnknown;
  }
}

MwStreamerVideoColorInfo MapColorInfo(const AVFrame& frame) {
  return {
      MapColorRange(frame.color_range),
      MapColorSpace(frame.colorspace),
      MapColorPrimaries(frame.color_primaries),
      MapColorTransfer(frame.color_trc),
      MapChromaLocation(frame.chroma_location),
  };
}

void ValidateAudioStorage(const AVFrame& frame) {
  if (frame.format != AV_SAMPLE_FMT_FLT ||
      av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame.format)) != 0 ||
      frame.sample_rate != kProcessorAudioSampleRate ||
      frame.ch_layout.nb_channels <= 0 || frame.nb_samples <= 0 ||
      !frame.extended_data || !frame.extended_data[0]) {
    throw std::invalid_argument(
        "Processor音频帧必须是48kHz float32交错有效数据");
  }
}

}  // namespace

VideoFrameAdapter::VideoFrameAdapter(const ffmpeg::Frame& frame) {
  if (!frame.get()) {
    throw std::invalid_argument("不能映射空视频Frame");
  }
  MapVideoBuffer(*frame.get(), &planes_, &view_.buffer);
  view_.color = MapColorInfo(*frame.get());
  view_.timestamp = MapTimestamp(*frame.get());
}

const MwStreamerVideoFrameView& VideoFrameAdapter::view() const noexcept {
  return view_;
}

VideoBufferAdapter::VideoBufferAdapter(ffmpeg::Frame& frame) {
  if (!frame.get()) {
    throw std::invalid_argument("不能映射空视频Frame");
  }
  MapVideoBuffer(*frame.get(), &planes_, &view_);
}

const MwStreamerVideoBufferView& VideoBufferAdapter::view() const noexcept {
  return view_;
}

AudioFrameAdapter::AudioFrameAdapter(const ffmpeg::Frame& frame) {
  if (!frame.get()) {
    throw std::invalid_argument("不能映射空音频Frame");
  }
  ValidateAudioStorage(*frame.get());
  view_ = {
      reinterpret_cast<const float*>(frame->extended_data[0]),
      static_cast<std::uint32_t>(frame->sample_rate),
      static_cast<std::uint32_t>(frame->ch_layout.nb_channels),
      static_cast<std::uint32_t>(frame->nb_samples),
      MapTimestamp(*frame.get()),
  };
}

const MwStreamerAudioFrameView& AudioFrameAdapter::view() const noexcept {
  return view_;
}

AudioBufferAdapter::AudioBufferAdapter(ffmpeg::Frame& frame) {
  if (!frame.get()) {
    throw std::invalid_argument("不能映射空音频Frame");
  }
  ValidateAudioStorage(*frame.get());
  view_ = {
      reinterpret_cast<float*>(frame->extended_data[0]),
      static_cast<std::uint32_t>(frame->ch_layout.nb_channels),
      static_cast<std::uint32_t>(frame->nb_samples),
  };
}

const MwStreamerAudioBufferView& AudioBufferAdapter::view() const noexcept {
  return view_;
}

}  // namespace mw::streamer::processor

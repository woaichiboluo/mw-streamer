#include "mw/frame_convter/opencv_cuda_convter.h"

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <nppi_color_conversion.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include "cuda_pixels.h"
#include "mw/frame_convter/internal/cuda_buffer_pool.h"
#include "mw/frame_convter/internal/cuda_frame_pool.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::frame_convter {
namespace {

namespace ffmpeg = streamer::ffmpeg;

using internal::CheckCuda;
using internal::CheckDriver;
using internal::ContextScope;
using internal::FramePool;
using internal::GpuMatBufferPool;

void CheckSize(int width, int height) {
  if (width <= 0 || height <= 0 || width % 2 || height % 2 ||
      width > INT_MAX / 3 || height > INT_MAX / 2 ||
      av_image_check_size(static_cast<unsigned int>(width),
                          static_cast<unsigned int>(height), 0, nullptr) < 0) {
    throw std::invalid_argument("CUDA 转换需要有效的偶数宽高");
  }
}

const AVHWFramesContext& FrameContext(const ffmpeg::Frame& frame) {
  if (!frame.get() || frame->format != AV_PIX_FMT_CUDA ||
      !frame->hw_frames_ctx || !frame->hw_frames_ctx->data) {
    throw std::invalid_argument("需要带 hw_frames_ctx 的 CUDA 帧");
  }
  const auto* context =
      reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  if (context->format != AV_PIX_FMT_CUDA || !context->device_ctx ||
      context->device_ctx->type != AV_HWDEVICE_TYPE_CUDA ||
      !context->device_ctx->hwctx || !context->device_ref ||
      (context->sw_format != AV_PIX_FMT_NV12 &&
       context->sw_format != AV_PIX_FMT_P010LE)) {
    throw std::invalid_argument("CUDA 转换仅支持 NV12/P010 硬件帧");
  }
  const auto* cuda =
      static_cast<const AVCUDADeviceContext*>(context->device_ctx->hwctx);
  if (!cuda->cuda_ctx) throw std::invalid_argument("CUDA 帧设备未初始化");
  return *context;
}

struct Color {
  float kr;
  float kb;
  float y_offset;
  float y_span;
  float c_offset;
  float c_span;
  float chroma_x;
  float chroma_y;
  bool p010;
};

Color ReadColor(const AVFrame& frame, AVPixelFormat format) {
  Color color{};
  color.p010 = format == AV_PIX_FMT_P010LE;
  switch (frame.colorspace) {
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_BT709:
      color.kr = 0.2126F;
      color.kb = 0.0722F;
      break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      color.kr = 0.299F;
      color.kb = 0.114F;
      break;
    case AVCOL_SPC_BT2020_NCL:
      color.kr = 0.2627F;
      color.kb = 0.0593F;
      break;
    default:
      throw std::invalid_argument("CUDA 转换需要受支持的原生颜色矩阵");
  }
  const float scale = color.p010 ? 4.0F : 1.0F;
  color.c_offset = 128.0F * scale;
  if (frame.color_range == AVCOL_RANGE_MPEG) {
    color.y_offset = 16.0F * scale;
    color.y_span = 219.0F * scale;
    color.c_span = 224.0F * scale;
  } else if (frame.color_range == AVCOL_RANGE_JPEG) {
    color.y_span = color.c_span = color.p010 ? 1023.0F : 255.0F;
  } else {
    throw std::invalid_argument("CUDA 转换需要原生 color_range");
  }
  switch (frame.chroma_location) {
    case AVCHROMA_LOC_LEFT:
      color.chroma_y = 0.5F;
      break;
    case AVCHROMA_LOC_CENTER:
      color.chroma_x = color.chroma_y = 0.5F;
      break;
    case AVCHROMA_LOC_TOPLEFT:
      break;
    case AVCHROMA_LOC_TOP:
      color.chroma_x = 0.5F;
      break;
    case AVCHROMA_LOC_BOTTOMLEFT:
      color.chroma_y = 1.0F;
      break;
    case AVCHROMA_LOC_BOTTOM:
      color.chroma_x = 0.5F;
      color.chroma_y = 1.0F;
      break;
    default:
      throw std::invalid_argument("CUDA 转换需要原生 chroma_location");
  }
  if (frame.color_trc == AVCOL_TRC_SMPTE2084 ||
      frame.color_trc == AVCOL_TRC_ARIB_STD_B67) {
    throw std::invalid_argument("HDR 与 RGB 转换需要独立颜色管理");
  }
  return color;
}

internal::ColorMatrix ColorMatrix(const Color& color, RgbOrder order,
                                  bool to_rgb) {
  if (order != RgbOrder::kRgb && order != RgbOrder::kBgr) {
    throw std::invalid_argument("RGB 通道顺序无效");
  }
  const float kr = color.kr;
  const float kb = color.kb;
  const float kg = 1.0F - kr - kb;
  internal::ColorMatrix matrix{};
  if (to_rgb) {
    const float y = 255.0F / color.y_span;
    const float c = 255.0F / color.c_span;
    matrix = {{{y, 0, 2 * (1 - kr) * c, 0},
               {y, -2 * kb * (1 - kb) / kg * c, -2 * kr * (1 - kr) / kg * c, 0},
               {y, 2 * (1 - kb) * c, 0, 0}}};
    for (auto& row : matrix.values) {
      row[3] = -row[0] * color.y_offset - (row[1] + row[2]) * color.c_offset;
    }
    if (order == RgbOrder::kBgr) {
      for (int column = 0; column < 4; ++column) {
        std::swap(matrix.values[0][column], matrix.values[2][column]);
      }
    }
  } else {
    const float y = color.y_span / 255.0F;
    const float c = color.c_span / 255.0F;
    matrix = {{{kr * y, kg * y, kb * y, color.y_offset},
               {-kr / (2 * (1 - kb)) * c, -kg / (2 * (1 - kb)) * c, 0.5F * c,
                color.c_offset},
               {0.5F * c, -kg / (2 * (1 - kr)) * c, -kb / (2 * (1 - kr)) * c,
                color.c_offset}}};
    if (order == RgbOrder::kBgr) {
      for (auto& row : matrix.values) std::swap(row[0], row[2]);
    }
  }
  return matrix;
}

void CheckStream(cudaStream_t stream, CUcontext expected) {
  if (!stream) return;
  CUcontext context = nullptr;
  CheckDriver(cuStreamGetCtx(stream, &context));
  if (context != expected) {
    throw std::invalid_argument("CUDA stream 与帧必须使用同一上下文");
  }
}

CUcontext CheckContext(const AVCUDADeviceContext& frame_device,
                       cudaStream_t stream) {
  // Validate before initializing a runtime context or doing GPU work. In
  // particular, an unbound caller must not create a different primary context.
  CUcontext current = nullptr;
  CheckDriver(cuCtxGetCurrent(&current));
  if (current != frame_device.cuda_ctx) {
    throw std::invalid_argument("OpenCV 与 FFmpeg 必须使用同一 CUDA 上下文");
  }
  CheckStream(stream, current);
  return current;
}

void CheckPointer(const void* pointer, CUcontext expected) {
  CUcontext actual = nullptr;
  if (!pointer ||
      cuPointerGetAttribute(&actual, CU_POINTER_ATTRIBUTE_CONTEXT,
                            reinterpret_cast<CUdeviceptr>(pointer)) !=
          CUDA_SUCCESS ||
      actual != expected) {
    throw std::invalid_argument("像素指针不属于预期 CUDA 上下文");
  }
}

int NppStep(std::size_t step) {
  if (step > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument("GPU 图像步长超过 NPP 限制");
  }
  return static_cast<int>(step);
}

void CheckNpp(NppStatus status) {
  if (status != NPP_SUCCESS) {
    throw std::runtime_error("NPP 颜色转换失败: " +
                             std::to_string(static_cast<int>(status)));
  }
}

NppStreamContext NppContext(cudaStream_t cuda_stream) {
  int device = 0;
  CheckCuda(cudaGetDevice(&device));
  cudaDeviceProp properties{};
  CheckCuda(cudaGetDeviceProperties(&properties, device));
  NppStreamContext stream{};
  stream.hStream = cuda_stream;
  stream.nCudaDeviceId = device;
  stream.nMultiProcessorCount = properties.multiProcessorCount;
  stream.nMaxThreadsPerMultiProcessor = properties.maxThreadsPerMultiProcessor;
  stream.nMaxThreadsPerBlock = properties.maxThreadsPerBlock;
  stream.nSharedMemPerBlock = properties.sharedMemPerBlock;
  stream.nCudaDevAttrComputeCapabilityMajor = properties.major;
  stream.nCudaDevAttrComputeCapabilityMinor = properties.minor;
  CheckCuda(cudaStreamGetFlags(cuda_stream, &stream.nStreamFlags));
  return stream;
}

// NPP's NV12 routines use centered 2x2 chroma blocks. Other sample locations
// and P010 packing require their own fused conversion, without intermediate
// float images. Dispatch depends only on the frame's native format/metadata.
bool UseNpp(const Color& color) {
  return !color.p010 && color.chroma_x == 0.5F && color.chroma_y == 0.5F;
}

bool Contains(const AVPixelFormat* formats, AVPixelFormat format) {
  if (!formats) return false;
  for (; *formats != AV_PIX_FMT_NONE; ++formats) {
    if (*formats == format) return true;
  }
  return false;
}

void ValidateUploadPixels(const ffmpeg::Frame& source) {
  if (!source.get()) throw std::invalid_argument("不能上传已移动的帧");
  const auto format = static_cast<AVPixelFormat>(source->format);
  const auto* description = av_pix_fmt_desc_get(format);
  if (!description || (description->flags & AV_PIX_FMT_FLAG_HWACCEL) ||
      source->hw_frames_ctx) {
    throw std::invalid_argument("上传需要 CPU 像素数据");
  }
  if (source->width <= 0 || source->height <= 0 ||
      av_image_check_size(static_cast<unsigned int>(source->width),
                          static_cast<unsigned int>(source->height), 0,
                          nullptr) < 0) {
    throw std::invalid_argument("上传帧尺寸无效");
  }
  if (source->width % (1 << description->log2_chroma_w) != 0 ||
      source->height % (1 << description->log2_chroma_h) != 0) {
    throw std::invalid_argument("CUDA 上传尺寸必须满足色度采样对齐");
  }
  int minimum_linesizes[4] = {};
  if (av_image_fill_linesizes(minimum_linesizes, format, source->width) < 0) {
    throw std::invalid_argument("无法确定上传帧平面布局");
  }
  for (int plane = 0; plane < 4; ++plane) {
    // FFmpeg's CUDA upload requires positive strides; CPU conversion also
    // supports bottom-up frames, so these checks stay local to the uploader.
    if (minimum_linesizes[plane] != 0 &&
        (!source->data[plane] ||
         source->linesize[plane] < minimum_linesizes[plane])) {
      throw std::invalid_argument("上传帧数据或平面步长无效");
    }
  }
}

CUcontext DeviceContext(const ffmpeg::HwDeviceContext& device) {
  if (!device.get() || !device.get()->data) {
    throw std::invalid_argument("转换器需要已初始化的 CUDA 设备");
  }
  const auto* hardware =
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
  if (hardware->type != AV_HWDEVICE_TYPE_CUDA || !hardware->hwctx) {
    throw std::invalid_argument("转换器需要已初始化的 CUDA 设备");
  }
  const auto* cuda = static_cast<const AVCUDADeviceContext*>(hardware->hwctx);
  if (!cuda->cuda_ctx) {
    throw std::invalid_argument("CUDA 设备上下文未初始化");
  }
  return cuda->cuda_ctx;
}

}  // namespace

class OpencvCudaConvter::Impl {
 public:
  Impl(const ffmpeg::HwDeviceContext& source, std::size_t pool_bytes)
      : device(source),
        context(DeviceContext(device)),
        buffers(context, pool_bytes) {}

  void CheckDevice(CUcontext actual) const {
    if (actual != context) {
      throw std::invalid_argument("CUDA 帧不属于转换器绑定的上下文");
    }
  }

  ffmpeg::HwDeviceContext device;
  CUcontext context;
  GpuMatBufferPool buffers;
  FramePool uploads;
  FramePool outputs;
};

OpencvCudaConvter::OpencvCudaConvter(const ffmpeg::HwDeviceContext& device,
                                     std::size_t pool_bytes)
    : impl_(std::make_unique<Impl>(device, pool_bytes)) {}

OpencvCudaConvter::~OpencvCudaConvter() = default;

ffmpeg::Frame OpencvCudaConvter::allocate(const ffmpeg::Frame& reference,
                                          int width, int height) {
  CheckSize(width, height);
  const auto& context = FrameContext(reference);
  const auto& device =
      *static_cast<const AVCUDADeviceContext*>(context.device_ctx->hwctx);
  impl_->CheckDevice(device.cuda_ctx);
  ContextScope context_scope(impl_->context);
  auto output =
      impl_->outputs.Get(impl_->device.get(), context.sw_format, width, height);
  output.CopyPropertiesFrom(reference);
  output.ClearCrop();
  context_scope.Finish();
  return output;
}

cv::cuda::GpuMat OpencvCudaConvter::ToMat(const ffmpeg::Frame& frame,
                                          RgbOrder order, cudaStream_t stream) {
  const auto& context = FrameContext(frame);
  CheckSize(frame->width, frame->height);
  if (frame->width > context.width || frame->height > context.height) {
    throw std::invalid_argument("CUDA 帧尺寸超过硬件帧池尺寸");
  }
  const auto color = ReadColor(*frame.get(), context.sw_format);
  const auto matrix = ColorMatrix(color, order, true);
  const auto& device =
      *static_cast<const AVCUDADeviceContext*>(context.device_ctx->hwctx);
  impl_->CheckDevice(device.cuda_ctx);
  CheckContext(device, stream);
  const auto bytes =
      static_cast<std::size_t>(frame->width) * (color.p010 ? 2U : 1U);
  for (int plane = 0; plane < 2; ++plane) {
    if (frame->linesize[plane] <= 0 ||
        static_cast<std::size_t>(frame->linesize[plane]) < bytes) {
      throw std::invalid_argument("CUDA 帧平面步长无效");
    }
    CheckPointer(frame->data[plane], device.cuda_ctx);
  }
  cv::cuda::GpuMat output(frame->height, frame->width, CV_8UC3,
                          &impl_->buffers);
  const std::uint8_t* y = frame->data[0];
  const std::uint8_t* uv = frame->data[1];
  const auto y_step = static_cast<std::size_t>(frame->linesize[0]);
  const auto uv_step = static_cast<std::size_t>(frame->linesize[1]);
  try {
    if (UseNpp(color)) {
      const Npp8u* planes[] = {y, uv};
      int steps[] = {NppStep(y_step), NppStep(uv_step)};
      // NPP shipped with CUDA 12.8 uses output-affine offsets, as verified by
      // matrix/range/channel-order tests. Its inverse-transform header instead
      // documents input offsets.
      CheckNpp(nppiNV12ToRGB_8u_ColorTwist32f_P2C3R_Ctx(
          planes, steps, output.data, NppStep(output.step),
          {frame->width, frame->height}, matrix.values, NppContext(stream)));
    } else {
      CheckCuda(internal::YuvToRgb(y, y_step, uv, uv_step, color.p010,
                                   frame->width, frame->height, color.chroma_x,
                                   color.chroma_y, matrix, output.data,
                                   output.step, stream));
    }
  } catch (...) {
    impl_->buffers.Retire(output);
    throw;
  }
  return output;
}

ffmpeg::Frame OpencvCudaConvter::ToFrame(const cv::cuda::GpuMat& image,
                                         const ffmpeg::Frame& reference,
                                         RgbOrder order, cudaStream_t stream) {
  if (image.empty() || image.type() != CV_8UC3 ||
      image.step < static_cast<std::size_t>(image.cols) * 3 ||
      image.step > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument("需要有效的 CV_8UC3 GpuMat");
  }
  CheckSize(image.cols, image.rows);
  const auto& context = FrameContext(reference);
  const auto color = ReadColor(*reference.get(), context.sw_format);
  const auto matrix = ColorMatrix(color, order, false);
  const auto& device =
      *static_cast<const AVCUDADeviceContext*>(context.device_ctx->hwctx);
  impl_->CheckDevice(device.cuda_ctx);
  const auto current = CheckContext(device, stream);
  CheckPointer(image.data, current);
  auto output = allocate(reference, image.cols, image.rows);
  auto* y = output->data[0];
  auto* uv = output->data[1];
  const auto y_step = static_cast<std::size_t>(output->linesize[0]);
  const auto uv_step = static_cast<std::size_t>(output->linesize[1]);
  try {
    if (UseNpp(color)) {
      Npp8u* planes[] = {y, uv};
      int steps[] = {NppStep(y_step), NppStep(uv_step)};
      CheckNpp(nppiRGBToNV12_8u_ColorTwist32f_C3P2R_Ctx(
          image.data, NppStep(image.step), planes, steps,
          {image.cols, image.rows}, matrix.values, NppContext(stream)));
    } else {
      CheckCuda(internal::RgbToYuv(image.data, image.step, y, y_step, uv,
                                   uv_step, color.p010, image.cols, image.rows,
                                   color.chroma_x, color.chroma_y, matrix,
                                   stream));
    }
  } catch (...) {
    impl_->outputs.Discard(output);
    throw;
  }
  return output;
}

ffmpeg::Frame OpencvCudaConvter::CpuFrameToCuda(const ffmpeg::Frame& source,
                                                cudaStream_t stream) {
  ValidateUploadPixels(source);
  ContextScope context_scope(impl_->context);
  CheckStream(stream, impl_->context);
  const auto format = static_cast<AVPixelFormat>(source->format);
  const auto free_constraints = [](AVHWFramesConstraints* value) {
    av_hwframe_constraints_free(&value);
  };
  const std::unique_ptr<AVHWFramesConstraints, decltype(free_constraints)>
      constraints(
          av_hwdevice_get_hwframe_constraints(impl_->device.get(), nullptr),
          free_constraints);
  if (!constraints) throw std::runtime_error("无法查询 CUDA 帧格式限制");
  if (!Contains(constraints->valid_sw_formats, format)) {
    throw std::invalid_argument("CUDA 不支持上传该 CPU 像素布局");
  }
  auto output = impl_->uploads.Get(impl_->device.get(), format, source->width,
                                   source->height);
  output.CopyPropertiesFrom(source);
  int row_bytes[4] = {};
  ffmpeg::FfmpegException::throwIfError(
      av_image_fill_linesizes(row_bytes, format, source->width),
      "查询 CUDA 上传平面步长");
  std::ptrdiff_t strides[4] = {};
  for (int plane = 0; plane < 4; ++plane) strides[plane] = row_bytes[plane];
  std::size_t sizes[4] = {};
  ffmpeg::FfmpegException::throwIfError(
      av_image_fill_plane_sizes(sizes, format, source->height, strides),
      "查询 CUDA 上传平面尺寸");
  try {
    for (int plane = 0; plane < 4; ++plane) {
      if (row_bytes[plane] == 0) continue;
      CheckCuda(cudaMemcpy2DAsync(
          output->data[plane],
          static_cast<std::size_t>(output->linesize[plane]),
          source->data[plane],
          static_cast<std::size_t>(source->linesize[plane]),
          static_cast<std::size_t>(row_bytes[plane]),
          sizes[plane] / static_cast<std::size_t>(row_bytes[plane]),
          cudaMemcpyHostToDevice, stream));
    }
    context_scope.Finish();
  } catch (...) {
    impl_->uploads.Discard(output);
    throw;
  }
  return output;
}

}  // namespace mw::frame_convter

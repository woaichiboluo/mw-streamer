#include "mw/frame_convter/opencv_cuda_convter.h"

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <nppi_color_conversion.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include "cuda_pixels.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::frame_convter {
namespace {

namespace ffmpeg = streamer::ffmpeg;

void CheckCuda(cudaError_t status) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string("CUDA 帧转换失败: ") +
                             cudaGetErrorString(status));
  }
}

void CheckDriver(CUresult status) {
  if (status != CUDA_SUCCESS) {
    const char* message = nullptr;
    cuGetErrorString(status, &message);
    throw std::runtime_error(std::string("CUDA 驱动操作失败: ") +
                             (message ? message : "未知错误"));
  }
}

class ContextScope final {
 public:
  explicit ContextScope(CUcontext context) {
    CheckDriver(cuCtxPushCurrent(context));
  }
  ~ContextScope() {
    if (active_) {
      CUcontext previous = nullptr;
      cuCtxPopCurrent(&previous);
    }
  }
  void Finish() {
    CUcontext previous = nullptr;
    const auto status = cuCtxPopCurrent(&previous);
    active_ = false;
    CheckDriver(status);
  }
  ContextScope(const ContextScope&) = delete;
  ContextScope& operator=(const ContextScope&) = delete;

 private:
  bool active_ = true;
};

// Declare after owned buffers so exceptional exits finish queued reads/writes
// before buffers are released or control returns to the owner of the inputs.
class ConversionCompletion final {
 public:
  explicit ConversionCompletion(CUstream stream = nullptr) : stream_(stream) {}
  ~ConversionCompletion() {
    if (pending_) cuStreamSynchronize(stream_);
  }
  ConversionCompletion(const ConversionCompletion&) = delete;
  ConversionCompletion& operator=(const ConversionCompletion&) = delete;

  void Finish() {
    CheckDriver(cuStreamSynchronize(stream_));
    pending_ = false;
  }

 private:
  CUstream stream_;
  bool pending_ = true;
};

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

CUcontext PrepareDevice(const AVCUDADeviceContext& frame_device) {
  // Validate before initializing a runtime context or doing GPU work. In
  // particular, an unbound caller must not create a different primary context.
  CUcontext current = nullptr;
  CheckDriver(cuCtxGetCurrent(&current));
  if (current != frame_device.cuda_ctx) {
    throw std::invalid_argument("OpenCV 与 FFmpeg 必须使用同一 CUDA 上下文");
  }
  CheckDriver(cuStreamSynchronize(frame_device.stream));
  // Includes work submitted by the caller on non-default OpenCV streams.
  CheckCuda(cudaDeviceSynchronize());
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

NppStreamContext NppContext() {
  int device = 0;
  CheckCuda(cudaGetDevice(&device));
  cudaDeviceProp properties{};
  CheckCuda(cudaGetDeviceProperties(&properties, device));
  NppStreamContext stream{};
  stream.nCudaDeviceId = device;
  stream.nMultiProcessorCount = properties.multiProcessorCount;
  stream.nMaxThreadsPerMultiProcessor = properties.maxThreadsPerMultiProcessor;
  stream.nMaxThreadsPerBlock = properties.maxThreadsPerBlock;
  stream.nSharedMemPerBlock = properties.sharedMemPerBlock;
  stream.nCudaDevAttrComputeCapabilityMajor = properties.major;
  stream.nCudaDevAttrComputeCapabilityMinor = properties.minor;
  CheckCuda(cudaStreamGetFlags(nullptr, &stream.nStreamFlags));
  return stream;
}

// NPP's NV12 routines use centered 2x2 chroma blocks. Other sample locations
// and P010 packing require their own fused conversion, without intermediate
// float images. Dispatch depends only on the frame's native format/metadata.
bool UseNpp(const Color& color) {
  return !color.p010 && color.chroma_x == 0.5F && color.chroma_y == 0.5F;
}

ffmpeg::Frame AllocateFrame(const ffmpeg::Frame& reference,
                            const AVHWFramesContext& context, int width,
                            int height) {
  const auto unref = [](AVBufferRef* value) { av_buffer_unref(&value); };
  const std::unique_ptr<AVBufferRef, decltype(unref)> pool(
      av_hwframe_ctx_alloc(context.device_ref), unref);
  if (!pool) throw std::bad_alloc();
  auto* output_context = reinterpret_cast<AVHWFramesContext*>(pool->data);
  output_context->format = AV_PIX_FMT_CUDA;
  output_context->sw_format = context.sw_format;
  output_context->width = width;
  output_context->height = height;
  ffmpeg::FfmpegException::throwIfError(av_hwframe_ctx_init(pool.get()),
                                        "初始化 CUDA 转换帧池");
  ffmpeg::Frame output;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_get_buffer(pool.get(), output.get(), 0), "分配 CUDA 转换帧");
  output.CopyPropertiesFrom(reference);
  output.ClearCrop();
  return output;
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

}  // namespace

cv::cuda::GpuMat OpencvCudaConvter::ToMat(const ffmpeg::Frame& frame,
                                          RgbOrder order) {
  const auto& context = FrameContext(frame);
  CheckSize(frame->width, frame->height);
  if (frame->width > context.width || frame->height > context.height) {
    throw std::invalid_argument("CUDA 帧尺寸超过硬件帧池尺寸");
  }
  const auto color = ReadColor(*frame.get(), context.sw_format);
  const auto matrix = ColorMatrix(color, order, true);
  const auto& device =
      *static_cast<const AVCUDADeviceContext*>(context.device_ctx->hwctx);
  PrepareDevice(device);
  const auto bytes =
      static_cast<std::size_t>(frame->width) * (color.p010 ? 2U : 1U);
  for (int plane = 0; plane < 2; ++plane) {
    if (frame->linesize[plane] <= 0 ||
        static_cast<std::size_t>(frame->linesize[plane]) < bytes) {
      throw std::invalid_argument("CUDA 帧平面步长无效");
    }
    CheckPointer(frame->data[plane], device.cuda_ctx);
  }
  cv::cuda::GpuMat output(frame->height, frame->width, CV_8UC3);
  ConversionCompletion completion;
  const std::uint8_t* y = frame->data[0];
  const std::uint8_t* uv = frame->data[1];
  const auto y_step = static_cast<std::size_t>(frame->linesize[0]);
  const auto uv_step = static_cast<std::size_t>(frame->linesize[1]);
  if (UseNpp(color)) {
    const Npp8u* planes[] = {y, uv};
    int steps[] = {NppStep(y_step), NppStep(uv_step)};
    // NPP shipped with CUDA 12.8 uses output-affine offsets, as verified by
    // matrix/range/channel-order tests. Its inverse-transform header instead
    // documents input offsets.
    CheckNpp(nppiNV12ToRGB_8u_ColorTwist32f_P2C3R_Ctx(
        planes, steps, output.data, NppStep(output.step),
        {frame->width, frame->height}, matrix.values, NppContext()));
  } else {
    CheckCuda(internal::YuvToRgb(y, y_step, uv, uv_step, color.p010,
                                 frame->width, frame->height, color.chroma_x,
                                 color.chroma_y, matrix, output.data,
                                 output.step, nullptr));
  }
  completion.Finish();
  return output;
}

ffmpeg::Frame OpencvCudaConvter::ToFrame(const cv::cuda::GpuMat& image,
                                         const ffmpeg::Frame& reference,
                                         RgbOrder order) {
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
  const auto current = PrepareDevice(device);
  CheckPointer(image.data, current);
  auto output = AllocateFrame(reference, context, image.cols, image.rows);
  ConversionCompletion completion;
  auto* y = output->data[0];
  auto* uv = output->data[1];
  const auto y_step = static_cast<std::size_t>(output->linesize[0]);
  const auto uv_step = static_cast<std::size_t>(output->linesize[1]);
  if (UseNpp(color)) {
    Npp8u* planes[] = {y, uv};
    int steps[] = {NppStep(y_step), NppStep(uv_step)};
    CheckNpp(nppiRGBToNV12_8u_ColorTwist32f_C3P2R_Ctx(
        image.data, NppStep(image.step), planes, steps,
        {image.cols, image.rows}, matrix.values, NppContext()));
  } else {
    CheckCuda(internal::RgbToYuv(
        image.data, image.step, y, y_step, uv, uv_step, color.p010, image.cols,
        image.rows, color.chroma_x, color.chroma_y, matrix, nullptr));
  }
  completion.Finish();
  return output;
}

ffmpeg::Frame OpencvCudaConvter::CpuFrameToCuda(
    const ffmpeg::Frame& source, const ffmpeg::HwDeviceContext& device) {
  ValidateUploadPixels(source);
  if (!device.get() || !device.get()->data ||
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data)->type !=
          AV_HWDEVICE_TYPE_CUDA) {
    throw std::invalid_argument("上传需要已初始化的 CUDA 设备");
  }
  auto device_ref = device.Ref();
  const auto* cuda = static_cast<const AVCUDADeviceContext*>(
      reinterpret_cast<const AVHWDeviceContext*>(device_ref.get()->data)
          ->hwctx);
  if (!cuda || !cuda->cuda_ctx) {
    throw std::invalid_argument("CUDA 设备上下文未初始化");
  }
  const auto format = static_cast<AVPixelFormat>(source->format);
  const auto free_constraints = [](AVHWFramesConstraints* value) {
    av_hwframe_constraints_free(&value);
  };
  const std::unique_ptr<AVHWFramesConstraints, decltype(free_constraints)>
      constraints(
          av_hwdevice_get_hwframe_constraints(device_ref.get(), nullptr),
          free_constraints);
  if (!constraints) throw std::runtime_error("无法查询 CUDA 帧格式限制");
  if (!Contains(constraints->valid_sw_formats, format)) {
    throw std::invalid_argument("CUDA 不支持上传该 CPU 像素布局");
  }
  const auto unref = [](AVBufferRef* value) { av_buffer_unref(&value); };
  const std::unique_ptr<AVBufferRef, decltype(unref)> pool(
      av_hwframe_ctx_alloc(device_ref.get()), unref);
  if (!pool) throw std::bad_alloc();
  auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
  frames->format = AV_PIX_FMT_CUDA;
  frames->sw_format = format;
  frames->width = source->width;
  frames->height = source->height;
  ffmpeg::FfmpegException::throwIfError(av_hwframe_ctx_init(pool.get()),
                                        "初始化 CUDA 上传帧池");
  AVPixelFormat* supported = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_transfer_get_formats(
          pool.get(), AV_HWFRAME_TRANSFER_DIRECTION_TO, &supported, 0),
      "查询 CUDA 上传支持格式");
  const std::unique_ptr<AVPixelFormat, decltype(&av_free)> supported_owner(
      supported, av_free);
  if (!Contains(supported, format)) {
    throw std::invalid_argument("CUDA 帧池不支持该上传格式");
  }
  ffmpeg::Frame output;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_get_buffer(pool.get(), output.get(), 0), "分配 CUDA 上传帧");
  ContextScope context_scope(cuda->cuda_ctx);
  ConversionCompletion upload(cuda->stream);
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_transfer_data(output.get(), source.get(), 0), "上传 CPU 帧");
  upload.Finish();
  context_scope.Finish();
  output.CopyPropertiesFrom(source);
  return output;
}

}  // namespace mw::frame_convter

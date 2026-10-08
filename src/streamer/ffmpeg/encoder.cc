#include "mw/streamer/ffmpeg/encoder.h"

#include <new>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

const AVCodec* FindEncoder(std::string_view encoder_name, AVMediaType type) {
  const std::string name(encoder_name);
  const auto* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) {
    throw FfmpegException(AVERROR_ENCODER_NOT_FOUND, "查找编码器 " + name);
  }
  if (codec->type != type) {
    throw std::invalid_argument("编码器类型与输出轨道类型不匹配");
  }
  return codec;
}

bool PositiveRatio(AVRational ratio) noexcept {
  return ratio.num > 0 && ratio.den > 0;
}

void ValidatePixelFormat(const AVCodecContext& context,
                         AVPixelFormat pixel_format) {
  const void* configurations = nullptr;
  int count = 0;
  FfmpegException::throwIfError(
      avcodec_get_supported_config(&context, nullptr,
                                   AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   &configurations, &count),
      "查询编码器像素格式");
  if (!configurations) return;
  const auto* formats = static_cast<const AVPixelFormat*>(configurations);
  for (int index = 0; index < count; ++index) {
    if (formats[index] == pixel_format) return;
  }
  throw std::invalid_argument("当前编码器不支持指定像素格式");
}

class Options final {
 public:
  explicit Options(AVDictionary* source) {
    const int result = av_dict_copy(&value_, source, 0);
    if (result < 0) {
      av_dict_free(&value_);
      FfmpegException::throwIfError(result, "复制编码器选项");
    }
  }
  ~Options() { av_dict_free(&value_); }

  Options(const Options&) = delete;
  Options& operator=(const Options&) = delete;

  AVDictionary* get() const noexcept { return value_; }
  AVDictionary** address() noexcept { return &value_; }

 private:
  AVDictionary* value_ = nullptr;
};

}  // namespace

Encoder::Encoder(std::string_view encoder_name, AVMediaType type)
    : context_(FindEncoder(encoder_name, type)) {}

Encoder::~Encoder() = default;

AVCodecContext* Encoder::context() noexcept { return context_.get(); }

const AVCodecContext* Encoder::context() const noexcept {
  return context_.get();
}

void Encoder::Open(AVDictionary* options) {
  const auto* codec = context()->codec;
  if ((codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
      !context()->hw_device_ctx) {
    throw std::invalid_argument("硬件编码器需要外部硬件设备上下文");
  }
  Options copied(options);
  const std::string_view name(codec->name);
  if (name.size() >= 6 && name.substr(name.size() - 6) == "_nvenc" &&
      !av_dict_get(copied.get(), "preset", nullptr, 0)) {
    FfmpegException::throwIfError(
        av_dict_set(copied.address(), "preset", "p2", 0), "设置NVENC预设");
  }
  FfmpegException::throwIfError(
      avcodec_open2(context(), codec, copied.address()), "avcodec_open2");
  if (const auto* unused =
          av_dict_get(copied.get(), "", nullptr, AV_DICT_IGNORE_SUFFIX)) {
    throw std::invalid_argument(std::string("未识别的编码器选项: ") +
                                unused->key);
  }
}

void VideoEncoder::Configure(const VideoEncoderConfig& config) {
  const auto* description = av_pix_fmt_desc_get(config.pixel_format);
  if (config.width <= 0 || config.height <= 0 || !description ||
      (description->flags & AV_PIX_FMT_FLAG_HWACCEL) ||
      !PositiveRatio(config.frame_rate) || !PositiveRatio(config.time_base) ||
      config.sample_aspect_ratio.num < 0 ||
      config.sample_aspect_ratio.den <= 0 || config.bit_rate < 0 ||
      config.gop_size < 0 || config.max_b_frames < 0) {
    throw std::invalid_argument("视频编码器配置无效");
  }
  auto* codec_context = context();
  ValidatePixelFormat(*codec_context, config.pixel_format);
  codec_context->width = config.width;
  codec_context->height = config.height;
  codec_context->pix_fmt = config.pixel_format;
  codec_context->framerate = config.frame_rate;
  codec_context->time_base = config.time_base;
  codec_context->bit_rate = config.bit_rate;
  codec_context->gop_size = config.gop_size;
  codec_context->max_b_frames = config.max_b_frames;
  codec_context->sample_aspect_ratio = config.sample_aspect_ratio;
  codec_context->color_range = config.color_range;
  codec_context->colorspace = config.color_space;
  codec_context->color_primaries = config.color_primaries;
  codec_context->color_trc = config.color_trc;
  codec_context->flags |= AV_CODEC_FLAG_FRAME_DURATION;
  if (config.global_header) codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

void VideoEncoder::ConfigureDevice(const VideoEncoderConfig& config,
                                   const HwDeviceContext& device) {
  if (!device.get()) {
    return;
  }
  const auto* native =
      reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
  if (native->type != AV_HWDEVICE_TYPE_CUDA) {
    throw std::invalid_argument("CUDA编码设备配置无效");
  }
  auto* codec_context = context();
  for (int index = 0;; ++index) {
    const auto* supported = avcodec_get_hw_config(codec_context->codec, index);
    if (!supported) {
      throw std::invalid_argument("指定编码器不支持CUDA设备");
    }
    if (supported->device_type == AV_HWDEVICE_TYPE_CUDA &&
        supported->pix_fmt == AV_PIX_FMT_CUDA &&
        (supported->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
                               AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))) {
      break;
    }
  }
  codec_context->pix_fmt = AV_PIX_FMT_CUDA;
  codec_context->hw_device_ctx = av_buffer_ref(device.get());
  if (!codec_context->hw_device_ctx) throw std::bad_alloc();
  codec_context->hw_frames_ctx =
      av_hwframe_ctx_alloc(codec_context->hw_device_ctx);
  if (!codec_context->hw_frames_ctx) throw std::bad_alloc();
  auto* frames =
      reinterpret_cast<AVHWFramesContext*>(codec_context->hw_frames_ctx->data);
  frames->format = AV_PIX_FMT_CUDA;
  frames->sw_format = config.pixel_format;
  frames->width = config.width;
  frames->height = config.height;
  FfmpegException::throwIfError(
      av_hwframe_ctx_init(codec_context->hw_frames_ctx),
      "初始化CUDA编码帧描述");
}

VideoEncoder::VideoEncoder(const VideoEncoderConfig& config,
                           AVDictionary* options)
    : Encoder(config.encoder_name, AVMEDIA_TYPE_VIDEO) {
  Configure(config);
  Open(options);
}

VideoEncoder::VideoEncoder(const VideoEncoderConfig& config,
                           const HwDeviceContext& device, AVDictionary* options)
    : Encoder(config.encoder_name, AVMEDIA_TYPE_VIDEO) {
  Configure(config);
  ConfigureDevice(config, device);
  Open(options);
}

Frame VideoEncoder::PrepareFrame(const Frame& source) const {
  const auto* codec = context();
  const auto* frame = source.get();
  if (!frame || !frame->data[0] || frame->width != codec->width ||
      frame->height != codec->height) {
    throw std::invalid_argument("视频搬运帧数据或尺寸不匹配");
  }
  const auto* target = codec->hw_frames_ctx
                           ? reinterpret_cast<const AVHWFramesContext*>(
                                 codec->hw_frames_ctx->data)
                           : nullptr;
  const auto* origin = frame->hw_frames_ctx
                           ? reinterpret_cast<const AVHWFramesContext*>(
                                 frame->hw_frames_ctx->data)
                           : nullptr;
  const auto layout = target ? target->sw_format : codec->pix_fmt;
  if ((origin ? origin->sw_format : frame->format) != layout ||
      (origin && frame->format != AV_PIX_FMT_CUDA)) {
    throw std::invalid_argument("视频搬运像素布局不匹配");
  }
  if (!target && !origin) return source.Ref();
  if (target && origin) {
    if (!origin->device_ref ||
        origin->device_ref->data != codec->hw_device_ctx->data) {
      throw std::invalid_argument("GPU视频帧不属于共享编码设备");
    }
    return source.Ref();
  }
  Frame result;
  if (target) {
    FfmpegException::throwIfError(
        av_hwframe_get_buffer(codec->hw_frames_ctx, result.get(), 0),
        "分配上传GPU帧");
  } else {
    result->format = layout;
  }
  FfmpegException::throwIfError(
      av_hwframe_transfer_data(result.get(), frame, 0), "搬运CPU/GPU视频帧");
  result.CopyPropertiesFrom(source);
  return result;
}

AudioEncoder::AudioEncoder(const AudioEncoderConfig& config,
                           AVDictionary* options)
    : Encoder(config.encoder_name, AVMEDIA_TYPE_AUDIO) {
  if (config.sample_rate <= 0 ||
      av_get_bytes_per_sample(config.sample_format) <= 0 ||
      !av_channel_layout_check(&config.channel_layout) || config.bit_rate < 0) {
    throw std::invalid_argument("音频编码器配置无效");
  }
  auto* codec_context = context();
  codec_context->sample_fmt = config.sample_format;
  codec_context->sample_rate = config.sample_rate;
  codec_context->time_base = {1, config.sample_rate};
  codec_context->bit_rate = config.bit_rate;
  FfmpegException::throwIfError(
      av_channel_layout_copy(&codec_context->ch_layout, &config.channel_layout),
      "复制音频编码声道布局");
  if (config.global_header) codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  Open(options);
}

int AudioEncoder::frame_size() const noexcept { return context()->frame_size; }

void Encoder::ValidateFrame(const AVFrame& frame) const {
  const auto* codec_context = context();
  if (frame.pts == AV_NOPTS_VALUE || !PositiveRatio(frame.time_base) ||
      frame.duration < 0 || frame.format < 0 || !frame.data[0]) {
    throw std::invalid_argument("编码输入Frame数据或时间戳无效");
  }
  if (codec_context->codec_type == AVMEDIA_TYPE_VIDEO) {
    if (frame.format != codec_context->pix_fmt ||
        frame.width != codec_context->width ||
        frame.height != codec_context->height) {
      throw std::invalid_argument("视频Frame格式或尺寸与编码配置不匹配");
    }
    if (codec_context->hw_frames_ctx) {
      if (!frame.hw_frames_ctx) {
        throw std::invalid_argument("CUDA编码需要GPU Frame");
      }
      const auto* expected = reinterpret_cast<const AVHWFramesContext*>(
          codec_context->hw_frames_ctx->data);
      const auto* actual =
          reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
      if (!actual->device_ref ||
          actual->device_ref->data != codec_context->hw_device_ctx->data ||
          actual->format != expected->format ||
          actual->sw_format != expected->sw_format) {
        throw std::invalid_argument("GPU Frame设备或底层像素格式不匹配");
      }
    }
    return;
  }
  if (frame.format != codec_context->sample_fmt ||
      frame.sample_rate != codec_context->sample_rate ||
      av_channel_layout_compare(&frame.ch_layout, &codec_context->ch_layout) !=
          0 ||
      frame.nb_samples <= 0) {
    throw std::invalid_argument("音频Frame格式与编码配置不匹配");
  }
  const int capabilities = codec_context->codec->capabilities;
  if (!(capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) &&
      (frame.nb_samples > codec_context->frame_size ||
       (frame.nb_samples < codec_context->frame_size &&
        !(capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)))) {
    throw std::invalid_argument("音频Frame样本数与编码帧大小不匹配");
  }
}

bool Encoder::SendFrame(const Frame& frame) {
  if (!frame.get()) {
    throw std::invalid_argument("编码输入Frame不能为空，请使用Drain提交EOF");
  }
  ValidateFrame(*frame.get());
  auto submitted = frame.Ref();
  submitted->pts =
      av_rescale_q(frame->pts, frame->time_base, context()->time_base);
  submitted->duration =
      av_rescale_q(frame->duration, frame->time_base, context()->time_base);
  submitted->time_base = context()->time_base;
  if (submitted->pts == AV_NOPTS_VALUE || submitted->duration < 0) {
    throw std::invalid_argument("编码时间戳换算溢出");
  }
  if (context()->codec_type == AVMEDIA_TYPE_VIDEO) {
    submitted->pict_type = AV_PICTURE_TYPE_NONE;
    submitted->pkt_dts = AV_NOPTS_VALUE;
  }
  const int result = avcodec_send_frame(context(), submitted.get());
  if (result == AVERROR(EAGAIN)) return false;
  FfmpegException::throwIfError(result, "avcodec_send_frame");
  return true;
}

EncodeResult Encoder::ReceivePacket(Packet& packet) {
  const int result = avcodec_receive_packet(context(), packet.get());
  if (result == AVERROR(EAGAIN)) return EncodeResult::kNeedInput;
  if (result == AVERROR_EOF) return EncodeResult::kEnd;
  FfmpegException::throwIfError(result, "avcodec_receive_packet");
  packet->time_base = context()->time_base;
  return EncodeResult::kPacket;
}

bool Encoder::Drain() {
  const int result = avcodec_send_frame(context(), nullptr);
  if (result == AVERROR(EAGAIN)) return false;
  if (result != AVERROR_EOF) {
    FfmpegException::throwIfError(result, "avcodec_send_frame EOF");
  }
  return true;
}

StreamInfo Encoder::stream_info(int stream_index) const {
  return StreamInfo::FromCodecContext(*context(), stream_index);
}

}  // namespace mw::streamer::ffmpeg

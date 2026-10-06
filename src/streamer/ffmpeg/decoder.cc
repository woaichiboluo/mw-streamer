#include "mw/streamer/ffmpeg/decoder.h"

#include <new>
#include <stdexcept>
#include <string>

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg {
namespace {

const AVCodec* FindDecoder(const StreamInfo& stream, AVMediaType type,
                           std::string_view decoder_name) {
  stream.Validate();
  const auto* parameters = stream.codec_parameters.get();
  if (parameters->codec_type != type) {
    throw std::invalid_argument("解码器类型与输入轨道类型不匹配");
  }
  const std::string name(decoder_name);
  const auto* codec = name.empty() ? avcodec_find_decoder(parameters->codec_id)
                                   : avcodec_find_decoder_by_name(name.c_str());
  if (!codec) {
    throw FfmpegException(AVERROR_DECODER_NOT_FOUND, "查找解码器 " + name);
  }
  if (codec->id != parameters->codec_id ||
      codec->type != parameters->codec_type) {
    throw std::invalid_argument("指定解码器与输入轨道编码不匹配");
  }
  return codec;
}

}  // namespace

Decoder::Decoder(const StreamInfo& stream, AVMediaType type,
                 std::string_view decoder_name)
    : context_(FindDecoder(stream, type, decoder_name)),
      time_base_(stream.time_base) {
  auto* context = context_.get();
  FfmpegException::throwIfError(
      avcodec_parameters_to_context(context, stream.codec_parameters.get()),
      "avcodec_parameters_to_context");
  context->pkt_timebase = time_base_;
  context->thread_count = 0;
}

AVCodecContext* Decoder::context() noexcept { return context_.get(); }

void Decoder::Open() {
  const auto* codec = context_.get()->codec;
  if ((codec->capabilities & AV_CODEC_CAP_HARDWARE) &&
      !context_.get()->hw_device_ctx) {
    throw std::invalid_argument("硬件解码器需要外部硬件设备上下文");
  }
  FfmpegException::throwIfError(avcodec_open2(context_.get(), codec, nullptr),
                                "avcodec_open2");
}

VideoDecoder::VideoDecoder(const StreamInfo& stream,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_VIDEO, decoder_name) {
  Open();
}

VideoDecoder::VideoDecoder(const StreamInfo& stream,
                           const HwDeviceContext& device,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_VIDEO, decoder_name) {
  auto* codec_context = context();
  if (device.get()) {
    const auto* native =
        reinterpret_cast<const AVHWDeviceContext*>(device.get()->data);
    if (native->type != AV_HWDEVICE_TYPE_CUDA) {
      throw std::invalid_argument("硬解只支持CUDA视频设备");
    }
    for (int index = 0;; ++index) {
      const auto* config = avcodec_get_hw_config(codec_context->codec, index);
      if (!config) {
        throw std::invalid_argument("指定解码器不支持CUDA设备");
      }
      if (config->device_type == native->type &&
          config->pix_fmt == AV_PIX_FMT_CUDA &&
          (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
        break;
      }
    }
    codec_context->hw_device_ctx = av_buffer_ref(device.get());
    if (!codec_context->hw_device_ctx) {
      throw std::bad_alloc();
    }
    codec_context->get_format = &VideoDecoder::GetHardwareFormat;
    // Frame threads add decode surfaces; automatic CPU threading can exceed
    // NVDEC's surface limit. CUDA performs decoding on the device.
    codec_context->thread_count = 2;
  }

  Open();
}

AudioDecoder::AudioDecoder(const StreamInfo& stream,
                           std::string_view decoder_name)
    : Decoder(stream, AVMEDIA_TYPE_AUDIO, decoder_name) {
  Open();
}

AVPixelFormat VideoDecoder::GetHardwareFormat(
    AVCodecContext*, const AVPixelFormat* formats) noexcept {
  for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    if (*format == AV_PIX_FMT_CUDA) {
      return *format;
    }
  }
  return AV_PIX_FMT_NONE;
}

bool Decoder::SendPacket(const Packet& packet) {
  if (!packet.get() || !packet->data || packet->size <= 0) {
    throw std::invalid_argument("解码输入Packet不能为空，请使用Drain提交EOF");
  }
  const int result = avcodec_send_packet(context_.get(), packet.get());
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  FfmpegException::throwIfError(result, "avcodec_send_packet");
  return true;
}

DecodeResult Decoder::ReceiveFrame(Frame& frame) {
  const int result = avcodec_receive_frame(context_.get(), frame.get());
  if (result == AVERROR(EAGAIN)) {
    return DecodeResult::kNeedInput;
  }
  if (result == AVERROR_EOF) {
    return DecodeResult::kEnd;
  }
  FfmpegException::throwIfError(result, "avcodec_receive_frame");
  if (context_.get()->hw_device_ctx &&
      (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx)) {
    frame.Unref();
    throw FfmpegException(AVERROR_INVALIDDATA, "硬解未交付硬件帧");
  }
  frame->time_base = time_base_;
  return DecodeResult::kFrame;
}

bool Decoder::Drain() {
  const int result = avcodec_send_packet(context_.get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  if (result != AVERROR_EOF) {
    FfmpegException::throwIfError(result, "avcodec_send_packet EOF");
  }
  return true;
}

void Decoder::Flush() noexcept { context_.FlushBuffers(); }

}  // namespace mw::streamer::ffmpeg

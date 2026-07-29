#include "mw/decoder/video_decoder.h"

#include <fmt/format.h>

#include <cerrno>
#include <cstddef>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/error.h"

namespace mw::streamer::decoder {
namespace {

const AVCodec* FindVideoDecoder(const ffmpeg::StreamInfo& stream_info,
                                const VideoDecoderConfig& config) {
  stream_info.Validate();
  const auto* parameters = stream_info.codec_parameters.get();
  if (parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
    throw std::invalid_argument("VideoDecoder只接受视频流");
  }

  const auto* codec =
      config.decoder_name.empty()
          ? avcodec_find_decoder(parameters->codec_id)
          : avcodec_find_decoder_by_name(config.decoder_name.c_str());
  if (!codec) {
    throw std::invalid_argument(fmt::format(
        "找不到视频解码器: codec_id={}, decoder_name={}",
        static_cast<int>(parameters->codec_id), config.decoder_name));
  }
  if (codec->type != AVMEDIA_TYPE_VIDEO || codec->id != parameters->codec_id) {
    throw std::invalid_argument(
        fmt::format("视频解码器与输入编码不匹配: decoder_name={}, "
                    "decoder_codec_id={}, input_codec_id={}",
                    codec->name, static_cast<int>(codec->id),
                    static_cast<int>(parameters->codec_id)));
  }
  if (config.backend == VideoDecoderBackend::kSoftware &&
      (codec->capabilities & AV_CODEC_CAP_HARDWARE) != 0) {
    throw std::invalid_argument(fmt::format(
        "软件解码不能使用硬件解码器: decoder_name={}", codec->name));
  }
  return codec;
}

bool SupportsCudaDeviceContext(const AVCodec* codec) {
  for (int index = 0;; ++index) {
    const auto* config = avcodec_get_hw_config(codec, index);
    if (!config) {
      return false;
    }
    if (config->device_type == AV_HWDEVICE_TYPE_CUDA &&
        config->pix_fmt == AV_PIX_FMT_CUDA &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
      return true;
    }
  }
}

bool IsHardwarePixelFormat(AVPixelFormat format) {
  const auto* descriptor = av_pix_fmt_desc_get(format);
  return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

}  // namespace

class VideoDecoder::Impl final {
 public:
  Impl(ffmpeg::StreamInfo stream_info, VideoDecoderConfig config)
      : stream_info_(std::move(stream_info)),
        config_(std::move(config)),
        codec_(FindVideoDecoder(stream_info_, config_)),
        context_(codec_) {
    if (config_.backend == VideoDecoderBackend::kCuda &&
        config_.device_index < 0) {
      throw std::invalid_argument("CUDA设备索引不能为负数");
    }
    if (config_.backend == VideoDecoderBackend::kCuda &&
        !SupportsCudaDeviceContext(codec_)) {
      throw std::invalid_argument(fmt::format(
          "视频解码器不支持CUDA设备上下文: decoder_name={}", codec_->name));
    }

    ffmpeg::ThrowIfError(
        avcodec_parameters_to_context(context_.get(),
                                      stream_info_.codec_parameters.get()),
        "复制视频解码参数");
    context_.get()->pkt_timebase = stream_info_.time_base;
    context_.get()->opaque = this;
    context_.get()->get_format = SelectPixelFormat;

    if (config_.backend == VideoDecoderBackend::kCuda) {
      hardware_context_.emplace(
          ffmpeg::HardwareContext::CreateCuda(config_.device_index));
      context_.get()->hw_device_ctx = av_buffer_ref(hardware_context_->get());
      if (!context_.get()->hw_device_ctx) {
        throw std::bad_alloc();
      }
    }

    ffmpeg::ThrowIfError(avcodec_open2(context_.get(), codec_, nullptr),
                         "打开视频解码器");
  }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  void Decode(const ffmpeg::Packet& packet) {
    const auto* raw_packet = packet.get();
    if (!raw_packet || raw_packet->stream_index != stream_info_.stream_index) {
      throw std::invalid_argument("视频AVPacket为空或stream_index不匹配");
    }
    if (drained_) {
      throw std::logic_error("VideoDecoder已Drain，必须Flush后才能继续解码");
    }

    SendPacket(raw_packet);
  }

  void Drain() {
    if (drained_) {
      return;
    }

    for (;;) {
      const auto result = avcodec_send_packet(context_.get(), nullptr);
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames() == 0) {
          throw std::runtime_error("视频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (result != AVERROR_EOF) {
        ffmpeg::ThrowIfError(result, "提交视频解码结束标记");
      }
      break;
    }

    ReceiveFrames();
    drained_ = true;
  }

  void Flush() {
    context_.FlushBuffers();
    frame_.Unref();
    drained_ = false;
  }

  const ffmpeg::StreamInfo& stream_info() const noexcept {
    return stream_info_;
  }

  const VideoDecoderConfig& config() const noexcept { return config_; }

  const ffmpeg::HardwareContext* hardware_context() const noexcept {
    return hardware_context_ ? &*hardware_context_ : nullptr;
  }

 private:
  static AVPixelFormat SelectPixelFormat(AVCodecContext* context,
                                         const AVPixelFormat* formats) {
    const auto* decoder = static_cast<const Impl*>(context->opaque);
    if (!decoder || !formats) {
      return AV_PIX_FMT_NONE;
    }

    for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
      if (decoder->config_.backend == VideoDecoderBackend::kCuda) {
        if (*format == AV_PIX_FMT_CUDA) {
          return *format;
        }
      } else if (!IsHardwarePixelFormat(*format)) {
        return *format;
      }
    }
    return AV_PIX_FMT_NONE;
  }

  void SendPacket(const AVPacket* packet) {
    for (;;) {
      const auto result = avcodec_send_packet(context_.get(), packet);
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames() == 0) {
          throw std::runtime_error("视频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      ffmpeg::ThrowIfError(result, "提交视频压缩包");
      break;
    }
    ReceiveFrames();
  }

  std::size_t ReceiveFrames() {
    std::size_t frame_count = 0;
    for (;;) {
      frame_.Unref();
      const auto result = avcodec_receive_frame(context_.get(), frame_.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return frame_count;
      }
      ffmpeg::ThrowIfError(result, "接收视频解码帧");
      ValidateFrame();
      ++frame_count;
      if (on_frame_) {
        on_frame_(frame_);
      }
    }
  }

  void ValidateFrame() const {
    const auto format = static_cast<AVPixelFormat>(frame_->format);
    if (config_.backend == VideoDecoderBackend::kCuda) {
      if (format != AV_PIX_FMT_CUDA || !frame_->hw_frames_ctx) {
        throw std::runtime_error("CUDA视频解码器输出了非CUDA帧");
      }
      return;
    }
    if (IsHardwarePixelFormat(format)) {
      throw std::runtime_error("软件视频解码器输出了硬件帧");
    }
  }

  ffmpeg::StreamInfo stream_info_;
  VideoDecoderConfig config_;
  const AVCodec* codec_ = nullptr;
  std::optional<ffmpeg::HardwareContext> hardware_context_;
  ffmpeg::CodecContext context_;
  ffmpeg::Frame frame_;
  OnFrame on_frame_;
  bool drained_ = false;
};

VideoDecoder::VideoDecoder(ffmpeg::StreamInfo stream_info,
                           VideoDecoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(stream_info), std::move(config))) {
}

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::SetOnFrame(OnFrame callback) {
  impl_->SetOnFrame(std::move(callback));
}

void VideoDecoder::Decode(const ffmpeg::Packet& packet) {
  impl_->Decode(packet);
}

void VideoDecoder::Drain() { impl_->Drain(); }

void VideoDecoder::Flush() { impl_->Flush(); }

const ffmpeg::StreamInfo& VideoDecoder::stream_info() const noexcept {
  return impl_->stream_info();
}

const VideoDecoderConfig& VideoDecoder::config() const noexcept {
  return impl_->config();
}

const ffmpeg::HardwareContext* VideoDecoder::hardware_context() const noexcept {
  return impl_->hardware_context();
}

}  // namespace mw::streamer::decoder

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
}

#include "mw/decoder/internal/codec_finder.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/pixel_format.h"
#include "mw/log/logging.h"
#include "mw/performance/internal/stopwatch.h"

namespace mw::streamer::decoder {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

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

}  // namespace

class VideoDecoder::Impl final {
 public:
  Impl(ffmpeg::StreamInfo stream_info, VideoDecoderConfig config)
      : stream_info_(std::move(stream_info)),
        config_(std::move(config)),
        codec_(internal::FindDecoder(stream_info_, config_.decoder_name,
                                     AVMEDIA_TYPE_VIDEO)),
        context_(codec_) {
    if (config_.backend == VideoDecoderBackend::kSoftware &&
        (codec_->capabilities & AV_CODEC_CAP_HARDWARE) != 0) {
      throw std::invalid_argument(fmt::format(
          "软件解码不能使用硬件解码器: decoder_name={}", codec_->name));
    }
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
    if (config_.backend == VideoDecoderBackend::kCuda) {
      Log::Info(
          "视频解码器已打开: stream_index={}, decoder_name={}, backend=cuda, "
          "device_index={}, dimensions={}x{}, time_base={}/{}",
          stream_info_.stream_index, codec_->name, config_.device_index,
          context_.get()->width, context_.get()->height,
          stream_info_.time_base.num, stream_info_.time_base.den);
    } else {
      Log::Info(
          "视频解码器已打开: stream_index={}, decoder_name={}, "
          "backend=software, dimensions={}x{}, time_base={}/{}",
          stream_info_.stream_index, codec_->name, context_.get()->width,
          context_.get()->height, stream_info_.time_base.num,
          stream_info_.time_base.den);
    }
  }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  VideoDecodeResult Decode(const ffmpeg::Packet& packet) {
    const auto* raw_packet = packet.get();
    if (!raw_packet || raw_packet->stream_index != stream_info_.stream_index) {
      throw std::invalid_argument("视频AVPacket为空或stream_index不匹配");
    }
    if (drained_) {
      throw std::logic_error("VideoDecoder已Drain，必须Flush后才能继续解码");
    }

    VideoDecodeResult result;
    performance::internal::Stopwatch stopwatch;
    SendPacket(raw_packet, result, stopwatch);
    result.service_time = stopwatch.elapsed();
    return result;
  }

  VideoDecodeResult Drain() {
    VideoDecodeResult decode_result;
    if (drained_) {
      return decode_result;
    }

    performance::internal::Stopwatch stopwatch;
    for (;;) {
      const auto result = stopwatch.Measure(
          [this]() { return avcodec_send_packet(context_.get(), nullptr); });
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames(&decode_result, &stopwatch) == 0) {
          throw std::runtime_error("视频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (result != AVERROR_EOF) {
        ffmpeg::ThrowIfError(result, "提交视频解码结束标记");
      }
      break;
    }

    ReceiveFrames(&decode_result, &stopwatch);
    decode_result.service_time = stopwatch.elapsed();
    drained_ = true;
    Log::Debug("视频解码器已排空: stream_index={}", stream_info_.stream_index);
    return decode_result;
  }

  void Flush() {
    context_.FlushBuffers();
    frame_.Unref();
    drained_ = false;
    Log::Debug("视频解码器已刷新: stream_index={}", stream_info_.stream_index);
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
      } else if (!ffmpeg::IsHardwarePixelFormat(*format)) {
        return *format;
      }
    }
    return AV_PIX_FMT_NONE;
  }

  void SendPacket(const AVPacket* packet, VideoDecodeResult& decode_result,
                  performance::internal::Stopwatch& stopwatch) {
    for (;;) {
      const auto result = stopwatch.Measure([this, packet]() {
        return avcodec_send_packet(context_.get(), packet);
      });
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames(&decode_result, &stopwatch) == 0) {
          throw std::runtime_error("视频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      ffmpeg::ThrowIfError(result, "提交视频压缩包");
      break;
    }
    ReceiveFrames(&decode_result, &stopwatch);
  }

  std::size_t ReceiveFrames(
      VideoDecodeResult* decode_result = nullptr,
      performance::internal::Stopwatch* stopwatch = nullptr) {
    std::size_t frame_count = 0;
    for (;;) {
      frame_.Unref();
      const auto result =
          stopwatch ? stopwatch->Measure([this]() {
            return avcodec_receive_frame(context_.get(), frame_.get());
          })
                    : avcodec_receive_frame(context_.get(), frame_.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return frame_count;
      }
      ffmpeg::ThrowIfError(result, "接收视频解码帧");
      if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame_->pts = frame_->best_effort_timestamp;
      }
      frame_->time_base = stream_info_.time_base;
      ValidateFrame();
      ++frame_count;
      if (decode_result) {
        ++decode_result->frames;
      }
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
    if (ffmpeg::IsHardwarePixelFormat(format)) {
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

VideoDecodeResult VideoDecoder::Decode(const ffmpeg::Packet& packet) {
  return impl_->Decode(packet);
}

VideoDecodeResult VideoDecoder::Drain() { return impl_->Drain(); }

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

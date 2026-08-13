#include "mw/encoder/video_encoder.h"

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <fmt/format.h>

#include "mw/encoder/internal/options.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/dictionary.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/ffmpeg/pixel_format.h"
#include "mw/log/logging.h"
#include "mw/media/internal/codec_bridge.h"

namespace mw::streamer::encoder {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

AVCodecID RequireSupportedCodec(MwStreamerCodec codec) {
  const auto codec_id = media::internal::ToAvCodecId(codec);
  if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_HEVC) {
    throw std::invalid_argument("VideoEncoder首版只支持H.264和H.265");
  }
  return codec_id;
}

const char* DefaultCudaEncoderName(AVCodecID codec_id) {
  switch (codec_id) {
    case AV_CODEC_ID_H264:
      return "h264_nvenc";
    case AV_CODEC_ID_HEVC:
      return "hevc_nvenc";
    default:
      throw std::logic_error("CUDA视频编码器codec无效");
  }
}

bool SupportsPixelFormat(const AVCodec& codec, AVPixelFormat pixel_format) {
  if (!codec.pix_fmts) {
    return true;
  }
  for (const auto* supported = codec.pix_fmts; *supported != AV_PIX_FMT_NONE;
       ++supported) {
    if (*supported == pixel_format) {
      return true;
    }
  }
  return false;
}

const AVCodec* FindEncoder(const VideoEncoderConfig& config,
                           AVPixelFormat pixel_format) {
  const auto codec_id = RequireSupportedCodec(config.codec);
  const bool cuda = pixel_format == AV_PIX_FMT_CUDA;
  const auto* codec =
      config.encoder_name.empty()
          ? (cuda ? avcodec_find_encoder_by_name(
                        DefaultCudaEncoderName(codec_id))
                  : avcodec_find_encoder(codec_id))
          : avcodec_find_encoder_by_name(config.encoder_name.c_str());
  if (!codec) {
    throw std::invalid_argument(
        fmt::format("找不到视频编码器: codec_id={}, encoder_name={}",
                    static_cast<int>(codec_id), config.encoder_name));
  }
  if (codec->type != AVMEDIA_TYPE_VIDEO || codec->id != codec_id) {
    throw std::invalid_argument(fmt::format(
        "视频编码器与配置不匹配: encoder_name={}, encoder_codec_id={}, "
        "configured_codec_id={}",
        codec->name, static_cast<int>(codec->id), static_cast<int>(codec_id)));
  }
  if (cuda && !SupportsPixelFormat(*codec, AV_PIX_FMT_CUDA)) {
    throw std::invalid_argument(fmt::format(
        "CUDA视频帧不能交给该编码器: encoder_name={}", codec->name));
  }
  if (!SupportsPixelFormat(*codec, pixel_format)) {
    throw std::invalid_argument(fmt::format(
        "视频编码器不支持输入像素格式: encoder_name={}, pixel_format={}",
        codec->name, static_cast<int>(pixel_format)));
  }
  return codec;
}

const AVHWFramesContext& ValidateCudaPrototype(const AVFrame& frame) {
  const auto* frames_context = ffmpeg::HardwareContext::GetFramesContext(frame);
  if (!frames_context ||
      frames_context->device_ctx->type != AV_HWDEVICE_TYPE_CUDA ||
      frames_context->width < frame.width ||
      frames_context->height < frame.height) {
    throw std::invalid_argument("CUDA视频帧包含无效的硬件帧上下文");
  }
  return *frames_context;
}

void ValidatePrototype(const AVFrame* frame) {
  if (!frame || frame->width <= 0 || frame->height <= 0 ||
      frame->format == AV_PIX_FMT_NONE || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0) {
    throw std::invalid_argument("VideoEncoder输入原型帧无效");
  }

  const auto pixel_format = static_cast<AVPixelFormat>(frame->format);
  if (pixel_format == AV_PIX_FMT_CUDA) {
    ValidateCudaPrototype(*frame);
  } else if (ffmpeg::IsHardwarePixelFormat(pixel_format)) {
    throw std::invalid_argument("VideoEncoder暂不支持非CUDA硬件视频帧");
  }
}

bool ParseInteger(std::string_view text, int* value) {
  if (!value || text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, *value);
  return result.ec == std::errc{} && result.ptr == end;
}

ffmpeg::Dictionary MakeOptions(const VideoEncoderConfig& config,
                               const char* encoder_name) {
  ffmpeg::Dictionary options;
  const bool is_nvenc =
      std::string_view(encoder_name).find("_nvenc") != std::string_view::npos;
  for (const auto& [key, value] : config.properties) {
    if (key.empty()) {
      Log::Warning("忽略键为空的视频编码器属性");
      continue;
    }
    if (key == "bf") {
      int b_frames = 0;
      if (!ParseInteger(value, &b_frames) || b_frames != 0) {
        Log::Warning(
            "首版不支持B帧，已忽略视频编码器属性: encoder_name={}, bf={}",
            encoder_name, value);
      }
      continue;
    }
    if (is_nvenc && key == "forced-idr") {
      if (value != "1") {
        Log::Warning(
            "NVENC恢复语义要求强制IDR，已覆盖视频编码器属性: "
            "encoder_name={}, forced-idr={}",
            encoder_name, value);
      }
      continue;
    }
    options.Set(key.c_str(), value.c_str());
  }
  options.Set("bf", "0");
  if (is_nvenc) {
    options.Set("forced-idr", "1");
  }
  return options;
}

}  // namespace

class VideoEncoder::Impl final {
 public:
  Impl(VideoEncoderConfig config, int stream_index)
      : config_(std::move(config)), stream_index_(stream_index) {
    RequireSupportedCodec(config_.codec);
    if (stream_index_ < 0) {
      throw std::invalid_argument("VideoEncoder stream_index不能为负数");
    }
    if (config_.frame_rate.den <= 0 || config_.frame_rate.num < 0) {
      throw std::invalid_argument("视频编码帧率必须为非负有理数");
    }
  }

  void Open(const ffmpeg::Frame& prototype) {
    if (context_) {
      throw std::logic_error("VideoEncoder只能打开一次");
    }
    ValidatePrototype(prototype.get());

    const auto* input = prototype.get();
    const auto pixel_format = static_cast<AVPixelFormat>(input->format);
    const auto* codec = FindEncoder(config_, pixel_format);
    ffmpeg::CodecContext pending_context(codec);
    auto* context = pending_context.get();
    context->width = input->width;
    context->height = input->height;
    context->pix_fmt = pixel_format;
    context->time_base = input->time_base;
    context->framerate = {
        config_.frame_rate.num,
        config_.frame_rate.den,
    };
    context->sample_aspect_ratio = input->sample_aspect_ratio;
    context->color_range = input->color_range;
    context->colorspace = input->colorspace;
    context->color_primaries = input->color_primaries;
    context->color_trc = input->color_trc;
    context->chroma_sample_location = input->chroma_location;
    context->max_b_frames = 0;

    if (pixel_format == AV_PIX_FMT_CUDA) {
      ValidateCudaPrototype(*input);
      context->hw_frames_ctx = av_buffer_ref(input->hw_frames_ctx);
      if (!context->hw_frames_ctx) {
        throw std::bad_alloc();
      }
    }

    auto options = MakeOptions(config_, codec->name);
    ffmpeg::ThrowIfError(avcodec_open2(context, codec, options.address()),
                         "打开视频编码器");
    internal::WarnUnusedOptions(options.get(), "视频", codec->name);
    if (context->max_b_frames != 0 || context->has_b_frames != 0) {
      throw std::runtime_error(fmt::format(
          "视频编码器未关闭B帧: encoder_name={}, max_b_frames={}, "
          "has_b_frames={}",
          codec->name, context->max_b_frames, context->has_b_frames));
    }

    stream_info_.emplace(
        ffmpeg::StreamInfo::FromCodecContext(*context, stream_index_));
    input_width_ = input->width;
    input_height_ = input->height;
    input_format_ = pixel_format;
    context_.emplace(std::move(pending_context));
    Log::Info(
        "视频编码器已打开: encoder_name={}, stream_index={}, width={}, "
        "height={}, pixel_format={}, frame_rate={}/{}, memory={}",
        codec->name, stream_index_, context->width, context->height,
        av_get_pix_fmt_name(context->pix_fmt), context->framerate.num,
        context->framerate.den,
        context->pix_fmt == AV_PIX_FMT_CUDA ? "CUDA" : "host");
  }

  void SetOnPacket(OnPacket callback) { on_packet_ = std::move(callback); }

  void Encode(const ffmpeg::Frame& frame, VideoEncodeMode mode) {
    RequireOpen();
    if (drained_) {
      throw std::logic_error("VideoEncoder已Drain，不能继续编码");
    }
    ValidateFrame(frame.get());

    if (mode == VideoEncodeMode::kAutomatic) {
      SendFrame(frame.get());
      return;
    }
    if (mode != VideoEncodeMode::kForceKeyFrame) {
      throw std::invalid_argument("VideoEncoder收到未知编码模式");
    }

    auto key_frame = frame.Ref();
    key_frame->pict_type = AV_PICTURE_TYPE_I;
    key_frame->key_frame = 1;
    SendFrame(key_frame.get());
  }

  void Drain() {
    RequireOpen();
    if (drained_) {
      return;
    }
    SendFrame(nullptr);
    drained_ = true;
    Log::Debug("视频编码器已排空: encoder_name={}, stream_index={}",
               context_->get()->codec->name, stream_index_);
  }

  bool is_open() const noexcept { return context_.has_value(); }

  const ffmpeg::StreamInfo& stream_info() const {
    if (!stream_info_) {
      throw std::logic_error("VideoEncoder打开前没有StreamInfo");
    }
    return *stream_info_;
  }

  const VideoEncoderConfig& config() const noexcept { return config_; }

 private:
  void RequireOpen() const {
    if (!context_) {
      throw std::logic_error("VideoEncoder尚未打开");
    }
  }

  void ValidateFrame(const AVFrame* frame) const {
    ValidatePrototype(frame);
    if (frame->width != input_width_ || frame->height != input_height_ ||
        frame->format != input_format_ ||
        av_cmp_q(frame->time_base, context_->get()->time_base) != 0) {
      throw std::invalid_argument(
          "当前VideoEncoder不支持动态改变尺寸、像素格式或time_base");
    }
    if (input_format_ == AV_PIX_FMT_CUDA &&
        (!frame->hw_frames_ctx ||
         frame->hw_frames_ctx->data != context_->get()->hw_frames_ctx->data)) {
      throw std::invalid_argument("当前VideoEncoder只接受打开时的CUDA硬件帧池");
    }
  }

  void SendFrame(const AVFrame* frame) {
    for (;;) {
      const int result = avcodec_send_frame(context_->get(), frame);
      if (result == AVERROR(EAGAIN)) {
        if (ReceivePackets() == 0) {
          throw std::runtime_error("视频编码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (frame == nullptr && result == AVERROR_EOF) {
        break;
      }
      ffmpeg::ThrowIfError(result,
                           frame ? "提交视频编码帧" : "提交视频编码结束标记");
      break;
    }
    ReceivePackets();
  }

  std::size_t ReceivePackets() {
    std::size_t packet_count = 0;
    for (;;) {
      packet_.Unref();
      const int result = avcodec_receive_packet(context_->get(), packet_.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return packet_count;
      }
      ffmpeg::ThrowIfError(result, "接收视频编码包");
      packet_->stream_index = stream_index_;
      ++packet_count;
      if (on_packet_) {
        on_packet_(packet_);
      }
    }
  }

  VideoEncoderConfig config_;
  int stream_index_ = 0;
  std::optional<ffmpeg::CodecContext> context_;
  std::optional<ffmpeg::StreamInfo> stream_info_;
  int input_width_ = 0;
  int input_height_ = 0;
  AVPixelFormat input_format_ = AV_PIX_FMT_NONE;
  ffmpeg::Packet packet_;
  OnPacket on_packet_;
  bool drained_ = false;
};

VideoEncoder::VideoEncoder(VideoEncoderConfig config, int stream_index)
    : impl_(std::make_unique<Impl>(std::move(config), stream_index)) {}

VideoEncoder::~VideoEncoder() = default;

void VideoEncoder::Open(const ffmpeg::Frame& prototype) {
  impl_->Open(prototype);
}

void VideoEncoder::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}

void VideoEncoder::Encode(const ffmpeg::Frame& frame, VideoEncodeMode mode) {
  impl_->Encode(frame, mode);
}

void VideoEncoder::Drain() { impl_->Drain(); }

bool VideoEncoder::is_open() const noexcept { return impl_->is_open(); }

const ffmpeg::StreamInfo& VideoEncoder::stream_info() const {
  return impl_->stream_info();
}

const VideoEncoderConfig& VideoEncoder::config() const noexcept {
  return impl_->config();
}

}  // namespace mw::streamer::encoder

#include "mw/encoder/audio_encoder.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <fmt/format.h>

#include "mw/encoder/internal/options.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/dictionary.h"
#include "mw/ffmpeg/error.h"
#include "mw/log/logging.h"

namespace mw::streamer::encoder {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

constexpr int kProcessorAudioSampleRate = 48000;
constexpr AVRational kAudioTimeBase{1, kProcessorAudioSampleRate};

const AVCodec* FindEncoder(const AudioEncoderConfig& config) {
  const auto* codec =
      config.encoder_name.empty()
          ? avcodec_find_encoder(AV_CODEC_ID_AAC)
          : avcodec_find_encoder_by_name(config.encoder_name.c_str());
  if (!codec) {
    throw std::invalid_argument(
        fmt::format("找不到AAC编码器: encoder_name={}", config.encoder_name));
  }
  if (codec->type != AVMEDIA_TYPE_AUDIO || codec->id != AV_CODEC_ID_AAC) {
    throw std::invalid_argument(
        fmt::format("音频编码器不是AAC编码器: encoder_name={}, codec_id={}",
                    codec->name, static_cast<int>(codec->id)));
  }
  return codec;
}

bool SupportsSampleRate(const AVCodec& codec, int sample_rate) {
  if (!codec.supported_samplerates) {
    return true;
  }
  for (const int* supported = codec.supported_samplerates; *supported != 0;
       ++supported) {
    if (*supported == sample_rate) {
      return true;
    }
  }
  return false;
}

bool SupportsSampleFormat(const AVCodec& codec, AVSampleFormat format) {
  if (!codec.sample_fmts) {
    return true;
  }
  for (const auto* supported = codec.sample_fmts;
       *supported != AV_SAMPLE_FMT_NONE; ++supported) {
    if (*supported == format) {
      return true;
    }
  }
  return false;
}

AVSampleFormat SelectSampleFormat(const AVCodec& codec) {
  if (SupportsSampleFormat(codec, AV_SAMPLE_FMT_FLT)) {
    return AV_SAMPLE_FMT_FLT;
  }
  if (SupportsSampleFormat(codec, AV_SAMPLE_FMT_FLTP)) {
    return AV_SAMPLE_FMT_FLTP;
  }
  if (codec.sample_fmts && codec.sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
    return codec.sample_fmts[0];
  }
  throw std::invalid_argument(
      fmt::format("AAC编码器没有可用采样格式: encoder_name={}", codec.name));
}

void ValidatePrototype(const AVFrame* frame) {
  if (!frame || frame->format != AV_SAMPLE_FMT_FLT ||
      av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)) !=
          0 ||
      frame->sample_rate != kProcessorAudioSampleRate ||
      av_channel_layout_check(&frame->ch_layout) != 1 ||
      frame->nb_samples <= 0 || !frame->extended_data ||
      !frame->extended_data[0] || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0) {
    throw std::invalid_argument(
        "AudioEncoder只接受48kHz float32交错有效音频帧");
  }
}

ffmpeg::Dictionary MakeOptions(const EncoderProperties& properties) {
  ffmpeg::Dictionary options;
  for (const auto& [key, value] : properties) {
    if (key.empty()) {
      Log::Warning("忽略键为空的AAC编码器属性");
      continue;
    }
    options.Set(key.c_str(), value.c_str());
  }
  return options;
}

}  // namespace

class AudioEncoder::Impl final {
 public:
  Impl(AudioEncoderConfig config, int stream_index)
      : config_(std::move(config)), stream_index_(stream_index) {
    if (stream_index_ < 0) {
      throw std::invalid_argument("AudioEncoder stream_index不能为负数");
    }
  }

  ~Impl() {
    av_audio_fifo_free(fifo_);
    swr_free(&resampler_);
    av_channel_layout_uninit(&input_layout_);
  }

  void Open(const ffmpeg::Frame& prototype) {
    if (context_) {
      throw std::logic_error("AudioEncoder只能打开一次");
    }
    ValidatePrototype(prototype.get());

    const auto* codec = FindEncoder(config_);
    if (!SupportsSampleRate(*codec, kProcessorAudioSampleRate)) {
      throw std::invalid_argument(fmt::format(
          "AAC编码器不支持48kHz采样率: encoder_name={}", codec->name));
    }

    ffmpeg::CodecContext pending_context(codec);
    auto* context = pending_context.get();
    context->sample_rate = kProcessorAudioSampleRate;
    context->sample_fmt = SelectSampleFormat(*codec);
    context->time_base = kAudioTimeBase;
    ffmpeg::ThrowIfError(av_channel_layout_copy(&context->ch_layout,
                                                &prototype.get()->ch_layout),
                         "复制AAC编码声道布局");

    auto options = MakeOptions(config_.properties);
    ffmpeg::ThrowIfError(avcodec_open2(context, codec, options.address()),
                         "打开AAC编码器");
    internal::WarnUnusedOptions(options.get(), "AAC", codec->name);

    SwrContext* pending_resampler = nullptr;
    AVAudioFifo* pending_fifo = nullptr;
    AVChannelLayout pending_input_layout{};
    try {
      ffmpeg::ThrowIfError(av_channel_layout_copy(&pending_input_layout,
                                                  &prototype.get()->ch_layout),
                           "保存AAC输入声道布局");
      ffmpeg::ThrowIfError(
          swr_alloc_set_opts2(
              &pending_resampler, &context->ch_layout, context->sample_fmt,
              context->sample_rate, &prototype.get()->ch_layout,
              static_cast<AVSampleFormat>(prototype.get()->format),
              prototype.get()->sample_rate, 0, nullptr),
          "配置AAC输入采样格式转换");
      ffmpeg::ThrowIfError(swr_init(pending_resampler),
                           "初始化AAC输入采样格式转换");
      pending_fifo = av_audio_fifo_alloc(
          context->sample_fmt, context->ch_layout.nb_channels,
          std::max(context->frame_size, prototype.get()->nb_samples));
      if (!pending_fifo) {
        throw std::bad_alloc();
      }
      stream_info_.emplace(
          ffmpeg::StreamInfo::FromCodecContext(*context, stream_index_));
    } catch (...) {
      av_audio_fifo_free(pending_fifo);
      swr_free(&pending_resampler);
      av_channel_layout_uninit(&pending_input_layout);
      throw;
    }

    input_layout_ = pending_input_layout;
    input_format_ = static_cast<AVSampleFormat>(prototype.get()->format);
    context_.emplace(std::move(pending_context));
    resampler_ = pending_resampler;
    fifo_ = pending_fifo;
    Log::Info(
        "AAC编码器已打开: encoder_name={}, stream_index={}, sample_rate={}, "
        "channels={}, sample_format={}, frame_size={}",
        codec->name, stream_index_, context->sample_rate,
        context->ch_layout.nb_channels,
        av_get_sample_fmt_name(context->sample_fmt), context->frame_size);
  }

  void SetOnPacket(OnPacket callback) { on_packet_ = std::move(callback); }

  void Encode(const ffmpeg::Frame& frame) {
    RequireOpen();
    if (drained_) {
      throw std::logic_error("AudioEncoder已Drain，不能继续编码");
    }
    ValidateFrame(frame.get());

    if (!next_frame_pts_) {
      next_frame_pts_ =
          frame->pts == AV_NOPTS_VALUE
              ? AV_NOPTS_VALUE
              : av_rescale_q(frame->pts, frame->time_base, kAudioTimeBase);
    }

    ConvertAndStore(*frame.get());
    EncodeAvailableFrames(false);
  }

  void Drain() {
    RequireOpen();
    if (drained_) {
      return;
    }

    DrainResampler();
    EncodeAvailableFrames(true);
    SendFrame(nullptr);
    drained_ = true;
    Log::Debug("AAC编码器已排空: encoder_name={}, stream_index={}",
               context_->get()->codec->name, stream_index_);
  }

  bool is_open() const noexcept { return context_.has_value(); }

  const ffmpeg::StreamInfo& stream_info() const {
    if (!stream_info_) {
      throw std::logic_error("AudioEncoder打开前没有StreamInfo");
    }
    return *stream_info_;
  }

  const AudioEncoderConfig& config() const noexcept { return config_; }

 private:
  void RequireOpen() const {
    if (!context_) {
      throw std::logic_error("AudioEncoder尚未打开");
    }
  }

  void ValidateFrame(const AVFrame* frame) const {
    ValidatePrototype(frame);
    if (frame->format != input_format_ ||
        av_channel_layout_compare(&frame->ch_layout, &input_layout_) != 0) {
      throw std::invalid_argument(
          "当前AudioEncoder不支持动态改变采样格式或声道布局");
    }
  }

  ffmpeg::Frame AllocateConvertedFrame(int sample_capacity) const {
    ffmpeg::Frame output;
    const auto* context = context_->get();
    output->format = context->sample_fmt;
    output->sample_rate = context->sample_rate;
    output->time_base = context->time_base;
    output->nb_samples = sample_capacity;
    ffmpeg::ThrowIfError(
        av_channel_layout_copy(&output->ch_layout, &context->ch_layout),
        "复制AAC转换输出声道布局");
    ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 0),
                         "分配AAC转换输出帧");
    return output;
  }

  void ConvertAndStore(const AVFrame& input) {
    const int output_capacity =
        swr_get_out_samples(resampler_, input.nb_samples);
    ffmpeg::ThrowIfError(output_capacity, "计算AAC转换输出容量");
    auto converted = AllocateConvertedFrame(std::max(output_capacity, 1));
    const int output_samples =
        swr_convert(resampler_, converted->extended_data, converted->nb_samples,
                    const_cast<const std::uint8_t**>(input.extended_data),
                    input.nb_samples);
    ffmpeg::ThrowIfError(output_samples, "转换AAC输入采样格式");
    Store(converted, output_samples);
  }

  void DrainResampler() {
    for (;;) {
      const int output_capacity = swr_get_out_samples(resampler_, 0);
      ffmpeg::ThrowIfError(output_capacity, "计算AAC转换尾部容量");
      if (output_capacity == 0) {
        return;
      }

      auto converted = AllocateConvertedFrame(output_capacity);
      const int output_samples =
          swr_convert(resampler_, converted->extended_data,
                      converted->nb_samples, nullptr, 0);
      ffmpeg::ThrowIfError(output_samples, "排空AAC输入采样格式转换");
      if (output_samples == 0) {
        return;
      }
      Store(converted, output_samples);
    }
  }

  void Store(ffmpeg::Frame& frame, int sample_count) {
    if (sample_count <= 0) {
      return;
    }
    const int current_size = av_audio_fifo_size(fifo_);
    ffmpeg::ThrowIfError(current_size, "读取AAC FIFO大小");
    ffmpeg::ThrowIfError(
        av_audio_fifo_realloc(fifo_, current_size + sample_count),
        "扩展AAC FIFO");
    const int written = av_audio_fifo_write(
        fifo_, reinterpret_cast<void**>(frame.get()->extended_data),
        sample_count);
    if (written != sample_count) {
      throw std::runtime_error("写入AAC FIFO的采样数量不完整");
    }
  }

  ffmpeg::Frame ReadFrame(int sample_count) {
    ffmpeg::Frame frame;
    const auto* context = context_->get();
    frame->format = context->sample_fmt;
    frame->sample_rate = context->sample_rate;
    frame->time_base = context->time_base;
    frame->nb_samples = sample_count;
    frame->pts = *next_frame_pts_;
    frame->duration = sample_count;
    ffmpeg::ThrowIfError(
        av_channel_layout_copy(&frame->ch_layout, &context->ch_layout),
        "复制AAC编码帧声道布局");
    ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 0), "分配AAC编码帧");

    const int read = av_audio_fifo_read(
        fifo_, reinterpret_cast<void**>(frame->extended_data), sample_count);
    if (read != sample_count) {
      throw std::runtime_error("读取AAC FIFO的采样数量不完整");
    }
    if (*next_frame_pts_ != AV_NOPTS_VALUE) {
      *next_frame_pts_ += sample_count;
    }
    return frame;
  }

  void EncodeAvailableFrames(bool include_partial) {
    const int frame_size = context_->get()->frame_size;
    for (;;) {
      const int available = av_audio_fifo_size(fifo_);
      ffmpeg::ThrowIfError(available, "读取AAC FIFO大小");
      if (available == 0 ||
          (!include_partial && frame_size > 0 && available < frame_size)) {
        return;
      }
      const int sample_count =
          frame_size > 0 ? std::min(available, frame_size) : available;
      auto frame = ReadFrame(sample_count);
      SendFrame(frame.get());
    }
  }

  void SendFrame(const AVFrame* frame) {
    for (;;) {
      const int result = avcodec_send_frame(context_->get(), frame);
      if (result == AVERROR(EAGAIN)) {
        if (ReceivePackets() == 0) {
          throw std::runtime_error("AAC编码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (frame == nullptr && result == AVERROR_EOF) {
        break;
      }
      ffmpeg::ThrowIfError(result,
                           frame ? "提交AAC编码帧" : "提交AAC编码结束标记");
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
      ffmpeg::ThrowIfError(result, "接收AAC编码包");
      packet_->stream_index = stream_index_;
      ++packet_count;
      if (on_packet_) {
        on_packet_(packet_);
      }
    }
  }

  AudioEncoderConfig config_;
  int stream_index_ = 0;
  std::optional<ffmpeg::CodecContext> context_;
  std::optional<ffmpeg::StreamInfo> stream_info_;
  SwrContext* resampler_ = nullptr;
  AVAudioFifo* fifo_ = nullptr;
  AVChannelLayout input_layout_{};
  AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
  std::optional<std::int64_t> next_frame_pts_;
  ffmpeg::Packet packet_;
  OnPacket on_packet_;
  bool drained_ = false;
};

AudioEncoder::AudioEncoder(AudioEncoderConfig config, int stream_index)
    : impl_(std::make_unique<Impl>(std::move(config), stream_index)) {}

AudioEncoder::~AudioEncoder() = default;

void AudioEncoder::Open(const ffmpeg::Frame& prototype) {
  impl_->Open(prototype);
}

void AudioEncoder::SetOnPacket(OnPacket callback) {
  impl_->SetOnPacket(std::move(callback));
}

void AudioEncoder::Encode(const ffmpeg::Frame& frame) { impl_->Encode(frame); }

void AudioEncoder::Drain() { impl_->Drain(); }

bool AudioEncoder::is_open() const noexcept { return impl_->is_open(); }

const ffmpeg::StreamInfo& AudioEncoder::stream_info() const {
  return impl_->stream_info();
}

const AudioEncoderConfig& AudioEncoder::config() const noexcept {
  return impl_->config();
}

}  // namespace mw::streamer::encoder

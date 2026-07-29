#include "mw/decoder/audio_decoder.h"

#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <fmt/format.h>

#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/error.h"

namespace mw::streamer::decoder {
namespace {

const AVCodec* FindAudioDecoder(const ffmpeg::StreamInfo& stream_info,
                                const AudioDecoderConfig& config) {
  stream_info.Validate();
  const auto* parameters = stream_info.codec_parameters.get();
  if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) {
    throw std::invalid_argument("AudioDecoder只接受音频流");
  }

  const auto* codec =
      config.decoder_name.empty()
          ? avcodec_find_decoder(parameters->codec_id)
          : avcodec_find_decoder_by_name(config.decoder_name.c_str());
  if (!codec) {
    throw std::invalid_argument(fmt::format(
        "找不到音频解码器: codec_id={}, decoder_name={}",
        static_cast<int>(parameters->codec_id), config.decoder_name));
  }
  if (codec->type != AVMEDIA_TYPE_AUDIO || codec->id != parameters->codec_id) {
    throw std::invalid_argument(
        fmt::format("音频解码器与输入编码不匹配: decoder_name={}, "
                    "decoder_codec_id={}, input_codec_id={}",
                    codec->name, static_cast<int>(codec->id),
                    static_cast<int>(parameters->codec_id)));
  }
  return codec;
}

}  // namespace

class AudioDecoder::Impl final {
 public:
  Impl(ffmpeg::StreamInfo stream_info, AudioDecoderConfig config)
      : stream_info_(std::move(stream_info)),
        config_(std::move(config)),
        context_(FindAudioDecoder(stream_info_, config_)) {
    const auto* codec = context_.get()->codec;
    ffmpeg::ThrowIfError(
        avcodec_parameters_to_context(context_.get(),
                                      stream_info_.codec_parameters.get()),
        "复制音频解码参数");
    context_.get()->pkt_timebase = stream_info_.time_base;
    ffmpeg::ThrowIfError(avcodec_open2(context_.get(), codec, nullptr),
                         "打开音频解码器");
  }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  void Decode(const ffmpeg::Packet& packet) {
    const auto* raw_packet = packet.get();
    if (!raw_packet || raw_packet->stream_index != stream_info_.stream_index) {
      throw std::invalid_argument("音频AVPacket为空或stream_index不匹配");
    }
    if (drained_) {
      throw std::logic_error("AudioDecoder已Drain，必须Flush后才能继续解码");
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
          throw std::runtime_error("音频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (result != AVERROR_EOF) {
        ffmpeg::ThrowIfError(result, "提交音频解码结束标记");
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

  const AudioDecoderConfig& config() const noexcept { return config_; }

 private:
  void SendPacket(const AVPacket* packet) {
    for (;;) {
      const auto result = avcodec_send_packet(context_.get(), packet);
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames() == 0) {
          throw std::runtime_error("音频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      ffmpeg::ThrowIfError(result, "提交音频压缩包");
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
      ffmpeg::ThrowIfError(result, "接收音频解码帧");
      ++frame_count;
      if (on_frame_) {
        on_frame_(frame_);
      }
    }
  }

  ffmpeg::StreamInfo stream_info_;
  AudioDecoderConfig config_;
  ffmpeg::CodecContext context_;
  ffmpeg::Frame frame_;
  OnFrame on_frame_;
  bool drained_ = false;
};

AudioDecoder::AudioDecoder(ffmpeg::StreamInfo stream_info,
                           AudioDecoderConfig config)
    : impl_(std::make_unique<Impl>(std::move(stream_info), std::move(config))) {
}

AudioDecoder::~AudioDecoder() = default;

void AudioDecoder::SetOnFrame(OnFrame callback) {
  impl_->SetOnFrame(std::move(callback));
}

void AudioDecoder::Decode(const ffmpeg::Packet& packet) {
  impl_->Decode(packet);
}

void AudioDecoder::Drain() { impl_->Drain(); }

void AudioDecoder::Flush() { impl_->Flush(); }

const ffmpeg::StreamInfo& AudioDecoder::stream_info() const noexcept {
  return impl_->stream_info();
}

const AudioDecoderConfig& AudioDecoder::config() const noexcept {
  return impl_->config();
}

}  // namespace mw::streamer::decoder

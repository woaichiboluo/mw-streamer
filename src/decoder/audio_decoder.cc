#include "mw/decoder/audio_decoder.h"

#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "mw/decoder/internal/codec_finder.h"
#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/error.h"
#include "mw/log/logging.h"
#include "mw/performance/internal/stopwatch.h"

namespace mw::streamer::decoder {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

}  // namespace

class AudioDecoder::Impl final {
 public:
  Impl(ffmpeg::StreamInfo stream_info, AudioDecoderConfig config)
      : stream_info_(std::move(stream_info)),
        config_(std::move(config)),
        context_(internal::FindDecoder(stream_info_, config_.decoder_name,
                                       AVMEDIA_TYPE_AUDIO)) {
    const auto* codec = context_.get()->codec;
    ffmpeg::ThrowIfError(
        avcodec_parameters_to_context(context_.get(),
                                      stream_info_.codec_parameters.get()),
        "复制音频解码参数");
    context_.get()->pkt_timebase = stream_info_.time_base;
    ffmpeg::ThrowIfError(avcodec_open2(context_.get(), codec, nullptr),
                         "打开音频解码器");
    Log::Info(
        "音频解码器已打开: stream_index={}, decoder_name={}, sample_rate={}, "
        "channels={}, time_base={}/{}",
        stream_info_.stream_index, codec->name, context_.get()->sample_rate,
        context_.get()->ch_layout.nb_channels, stream_info_.time_base.num,
        stream_info_.time_base.den);
  }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  AudioDecodeResult Decode(const ffmpeg::Packet& packet) {
    const auto* raw_packet = packet.get();
    if (!raw_packet || raw_packet->stream_index != stream_info_.stream_index) {
      throw std::invalid_argument("音频AVPacket为空或stream_index不匹配");
    }
    if (drained_) {
      throw std::logic_error("AudioDecoder已Drain，必须Flush后才能继续解码");
    }

    AudioDecodeResult result;
    performance::internal::Stopwatch stopwatch;
    SendPacket(raw_packet, result, stopwatch);
    result.service_time = stopwatch.elapsed();
    return result;
  }

  AudioDecodeResult Drain() {
    AudioDecodeResult decode_result;
    if (drained_) {
      return decode_result;
    }

    performance::internal::Stopwatch stopwatch;
    for (;;) {
      const auto result = stopwatch.Measure(
          [this]() { return avcodec_send_packet(context_.get(), nullptr); });
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames(&decode_result, &stopwatch) == 0) {
          throw std::runtime_error("音频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      if (result != AVERROR_EOF) {
        ffmpeg::ThrowIfError(result, "提交音频解码结束标记");
      }
      break;
    }

    ReceiveFrames(&decode_result, &stopwatch);
    decode_result.service_time = stopwatch.elapsed();
    drained_ = true;
    Log::Debug("音频解码器已排空: stream_index={}", stream_info_.stream_index);
    return decode_result;
  }

  void Flush() {
    context_.FlushBuffers();
    frame_.Unref();
    drained_ = false;
    Log::Debug("音频解码器已刷新: stream_index={}", stream_info_.stream_index);
  }

  const ffmpeg::StreamInfo& stream_info() const noexcept {
    return stream_info_;
  }

  const AudioDecoderConfig& config() const noexcept { return config_; }

 private:
  void SendPacket(const AVPacket* packet, AudioDecodeResult& decode_result,
                  performance::internal::Stopwatch& stopwatch) {
    for (;;) {
      const auto result = stopwatch.Measure([this, packet]() {
        return avcodec_send_packet(context_.get(), packet);
      });
      if (result == AVERROR(EAGAIN)) {
        if (ReceiveFrames(&decode_result, &stopwatch) == 0) {
          throw std::runtime_error("音频解码器send和receive同时返回EAGAIN");
        }
        continue;
      }
      ffmpeg::ThrowIfError(result, "提交音频压缩包");
      break;
    }
    ReceiveFrames(&decode_result, &stopwatch);
  }

  std::size_t ReceiveFrames(
      AudioDecodeResult* decode_result = nullptr,
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
      ffmpeg::ThrowIfError(result, "接收音频解码帧");
      ++frame_count;
      if (decode_result) {
        decode_result->samples +=
            static_cast<std::uint64_t>(frame_->nb_samples);
      }
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

AudioDecodeResult AudioDecoder::Decode(const ffmpeg::Packet& packet) {
  return impl_->Decode(packet);
}

AudioDecodeResult AudioDecoder::Drain() { return impl_->Drain(); }

void AudioDecoder::Flush() { impl_->Flush(); }

const ffmpeg::StreamInfo& AudioDecoder::stream_info() const noexcept {
  return impl_->stream_info();
}

const AudioDecoderConfig& AudioDecoder::config() const noexcept {
  return impl_->config();
}

}  // namespace mw::streamer::decoder

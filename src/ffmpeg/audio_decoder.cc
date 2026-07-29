#include "mw/ffmpeg/audio_decoder.h"

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

namespace mw::streamer::ffmpeg {
namespace {

const AVCodec* FindAudioDecoder(const StreamInfo& stream_info) {
  stream_info.Validate();
  const auto* parameters = stream_info.codec_parameters.get();
  if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) {
    throw std::invalid_argument("AudioDecoder只接受音频流");
  }
  const auto* codec = avcodec_find_decoder(parameters->codec_id);
  if (!codec) {
    throw std::invalid_argument(
        fmt::format("找不到音频解码器: codec_id={}",
                    static_cast<int>(parameters->codec_id)));
  }
  return codec;
}

}  // namespace

class AudioDecoder::Impl final {
 public:
  explicit Impl(StreamInfo stream_info)
      : stream_info_(std::move(stream_info)),
        context_(FindAudioDecoder(stream_info_)) {
    const auto* codec = context_.get()->codec;
    ThrowIfError(avcodec_parameters_to_context(
                     context_.get(), stream_info_.codec_parameters.get()),
                 "复制音频解码参数");
    context_.get()->pkt_timebase = stream_info_.time_base;
    ThrowIfError(avcodec_open2(context_.get(), codec, nullptr),
                 "打开音频解码器");
  }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  void Decode(const Packet& packet) {
    const auto* raw_packet = packet.get();
    if (!raw_packet || raw_packet->stream_index != stream_info_.stream_index) {
      throw std::invalid_argument("音频AVPacket为空或stream_index不匹配");
    }
    if (drained_) {
      throw std::logic_error("AudioDecoder已Drain，必须Reset后才能继续解码");
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
        ThrowIfError(result, "提交音频解码结束标记");
      }
      break;
    }

    ReceiveFrames();
    drained_ = true;
  }

  void Reset() {
    context_.FlushBuffers();
    frame_.Unref();
    drained_ = false;
  }

  const StreamInfo& stream_info() const noexcept { return stream_info_; }

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
      ThrowIfError(result, "提交音频压缩包");
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
      ThrowIfError(result, "接收音频解码帧");
      ++frame_count;
      if (on_frame_) {
        on_frame_(frame_);
      }
    }
  }

  StreamInfo stream_info_;
  CodecContext context_;
  Frame frame_;
  OnFrame on_frame_;
  bool drained_ = false;
};

AudioDecoder::AudioDecoder(StreamInfo stream_info)
    : impl_(std::make_unique<Impl>(std::move(stream_info))) {}

AudioDecoder::~AudioDecoder() = default;

void AudioDecoder::SetOnFrame(OnFrame callback) {
  impl_->SetOnFrame(std::move(callback));
}

void AudioDecoder::Decode(const Packet& packet) { impl_->Decode(packet); }

void AudioDecoder::Drain() { impl_->Drain(); }

void AudioDecoder::Reset() { impl_->Reset(); }

const StreamInfo& AudioDecoder::stream_info() const noexcept {
  return impl_->stream_info();
}

}  // namespace mw::streamer::ffmpeg

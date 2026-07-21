#include "mw/streamer/internal/audio_auto_decoder.h"

#include <cerrno>
#include <optional>
#include <utility>

#include "fmt/format.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/stream_info.h"

namespace mw::streamer::internal {

void AudioAutoDecoder::Open(const StreamInfo& stream) {
  if (is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioAutoDecoder can only be opened once");
  }

  const AVCodecParameters* parameters = stream.codec_parameters.get();
  if (parameters == nullptr || parameters->codec_type != AVMEDIA_TYPE_AUDIO ||
      stream.index < 0 || !IsValidTimeBase(stream.time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioAutoDecoder requires a valid audio stream");
  }

  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  if (codec == nullptr) {
    ThrowError(StatusCode::kUnsupported,
               fmt::format("audio decoder is unavailable for codec id {}",
                           static_cast<int>(parameters->codec_id)));
  }

  ffmpeg::CodecContext candidate(codec);
  CheckFfmpeg(avcodec_parameters_to_context(candidate.get(), parameters),
              "failed to copy audio stream parameters");
  candidate.get()->pkt_timebase = stream.time_base;
  CheckFfmpeg(avcodec_open2(candidate.get(), codec, nullptr),
              "failed to open the audio decoder");

  packet_time_base_ = stream.time_base;
  stream_index_ = stream.index;
  decoder_.emplace(std::move(candidate));
}

bool AudioAutoDecoder::Send(const ffmpeg::Packet& packet) {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioAutoDecoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot send audio after end of stream");
  }

  const AVPacket* raw_packet = packet.get();
  if (raw_packet == nullptr || raw_packet->data == nullptr ||
      raw_packet->size <= 0 || raw_packet->stream_index != stream_index_) {
    ThrowError(StatusCode::kInvalidArgument,
               "packet does not belong to this audio stream");
  }

  const int result = avcodec_send_packet(decoder_->get(), raw_packet);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send a packet to the audio decoder");
  return true;
}

bool AudioAutoDecoder::SendEndOfStream() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioAutoDecoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "audio end of stream was already sent");
  }

  const int result = avcodec_send_packet(decoder_->get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send the audio drain packet");
  drain_sent_ = true;
  return true;
}

std::optional<ffmpeg::Frame> AudioAutoDecoder::Receive() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioAutoDecoder has not been opened");
  }

  ffmpeg::Frame frame;
  const int result = avcodec_receive_frame(decoder_->get(), frame.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return std::nullopt;
  }
  CheckFfmpeg(result, "failed to receive a decoded audio frame");
  frame.get()->time_base = packet_time_base_;
  return frame;
}

bool AudioAutoDecoder::is_open() const noexcept { return decoder_.has_value(); }

int AudioAutoDecoder::stream_index() const noexcept { return stream_index_; }

}  // namespace mw::streamer::internal

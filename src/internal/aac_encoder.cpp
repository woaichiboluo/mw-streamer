#include "mw/streamer/internal/aac_encoder.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kAudioEncoderTimeBase{1, kProcessorAudioSampleRate};

ffmpeg::Frame MakeEncoderFrame(const NormalizedAudioFrame& input,
                               std::int64_t pts,
                               const AVChannelLayout& channel_layout) {
  ffmpeg::Frame frame;
  AVFrame* raw_frame = frame.get();
  raw_frame->format = AV_SAMPLE_FMT_FLTP;
  raw_frame->sample_rate = kProcessorAudioSampleRate;
  raw_frame->nb_samples = static_cast<int>(kProcessorAudioBlockFrameCount);
  raw_frame->pts = pts;
  raw_frame->time_base = kAudioEncoderTimeBase;
  CheckFfmpeg(av_channel_layout_copy(&raw_frame->ch_layout, &channel_layout),
              "failed to copy the AAC channel layout");
  CheckFfmpeg(av_frame_get_buffer(raw_frame, 0),
              "failed to allocate an AAC input frame");
  frame.MakeWritable();

  auto* left = reinterpret_cast<float*>(raw_frame->data[0]);
  auto* right = reinterpret_cast<float*>(raw_frame->data[1]);
  for (std::size_t index = 0; index < kProcessorAudioBlockFrameCount; ++index) {
    left[index] = input.samples[index * kProcessorAudioChannelCount];
    right[index] = input.samples[index * kProcessorAudioChannelCount + 1];
  }
  return frame;
}

}  // namespace

void AacEncoder::Open(std::int64_t bit_rate) {
  if (is_open()) {
    ThrowError(StatusCode::kInvalidArgument,
               "AacEncoder can only be opened once");
  }
  if (bit_rate <= 0) {
    ThrowError(StatusCode::kInvalidArgument, "AAC bitrate must be positive");
  }

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (codec == nullptr || av_codec_is_encoder(codec) == 0) {
    ThrowError(StatusCode::kUnsupported, "AAC encoder is unavailable");
  }

  ffmpeg::CodecContext candidate(codec);
  AVCodecContext* context = candidate.get();
  context->codec_type = AVMEDIA_TYPE_AUDIO;
  context->codec_id = AV_CODEC_ID_AAC;
  context->profile = AV_PROFILE_AAC_LOW;
  context->sample_fmt = AV_SAMPLE_FMT_FLTP;
  context->sample_rate = kProcessorAudioSampleRate;
  context->time_base = kAudioEncoderTimeBase;
  context->bit_rate = bit_rate;
  context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  av_channel_layout_default(&context->ch_layout, kProcessorAudioChannelCount);
  CheckFfmpeg(avcodec_open2(context, codec, nullptr),
              "failed to open the AAC encoder");
  if (context->frame_size != static_cast<int>(kProcessorAudioBlockFrameCount)) {
    ThrowError(StatusCode::kUnsupported,
               "AAC encoder does not use 1024-sample frames");
  }

  CheckFfmpeg(avcodec_parameters_from_context(codec_parameters_.get(), context),
              "failed to copy AAC codec parameters");
  encoder_.emplace(std::move(candidate));
}

bool AacEncoder::Send(const NormalizedAudioFrame& frame) {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "AacEncoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot send an audio frame after end of stream");
  }
  if (frame.frame_count != kProcessorAudioBlockFrameCount ||
      frame.samples.size() !=
          kProcessorAudioBlockFrameCount * kProcessorAudioChannelCount) {
    ThrowError(StatusCode::kInvalidArgument,
               "AAC input must contain 1024 stereo frames");
  }

  ffmpeg::Frame encoder_frame =
      MakeEncoderFrame(frame, next_pts_, encoder_->get()->ch_layout);
  const int result = avcodec_send_frame(encoder_->get(), encoder_frame.get());
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send a frame to the AAC encoder");
  next_pts_ += static_cast<std::int64_t>(kProcessorAudioBlockFrameCount);
  return true;
}

bool AacEncoder::SendEndOfStream() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "AacEncoder has not been opened");
  }
  if (drain_sent_) {
    ThrowError(StatusCode::kInvalidArgument,
               "AAC end of stream was already sent");
  }

  const int result = avcodec_send_frame(encoder_->get(), nullptr);
  if (result == AVERROR(EAGAIN)) {
    return false;
  }
  CheckFfmpeg(result, "failed to send the AAC drain frame");
  drain_sent_ = true;
  return true;
}

std::optional<ffmpeg::Packet> AacEncoder::Receive() {
  if (!is_open()) {
    ThrowError(StatusCode::kInvalidArgument, "AacEncoder has not been opened");
  }

  ffmpeg::Packet packet;
  const int result = avcodec_receive_packet(encoder_->get(), packet.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
    return std::nullopt;
  }
  CheckFfmpeg(result, "failed to receive an AAC packet");
  packet.get()->time_base = kAudioEncoderTimeBase;
  return packet;
}

bool AacEncoder::is_open() const noexcept {
  return encoder_.has_value() && encoder_->get() != nullptr;
}

AVRational AacEncoder::time_base() const noexcept {
  return kAudioEncoderTimeBase;
}

const ffmpeg::CodecParameters& AacEncoder::codec_parameters() const noexcept {
  return codec_parameters_;
}

}  // namespace mw::streamer::internal

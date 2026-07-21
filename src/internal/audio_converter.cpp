#include "mw/streamer/internal/audio_converter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "mw/streamer/ffmpeg/channel_layout.h"
#include "mw/streamer/ffmpeg/swr_context.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kOutputTimeBase{1, kProcessorAudioSampleRate};

}  // namespace

struct AudioConverter::State {
  explicit State(const AVChannelLayout& channel_layout)
      : source_channel_layout(ffmpeg::ChannelLayout::CopyOf(channel_layout)) {}

  ffmpeg::SwrContext context;
  ffmpeg::ChannelLayout source_channel_layout;
  AVSampleFormat source_sample_format = AV_SAMPLE_FMT_NONE;
  int source_sample_rate = 0;
  int64_t source_base_pts = MW_TIMESTAMP_UNAVAILABLE;
  int64_t source_base_pts_us = MW_TIMESTAMP_UNAVAILABLE;
  AVRational source_time_base{0, 1};
  int64_t source_base_output_frame = 0;
  int64_t total_output_frames = 0;
  bool flushed = false;
};

MwAudioFrameView NormalizedAudioFrame::view() noexcept {
  MwAudioFrameView result{};
  result.samples = samples.data();
  result.frame_count = frame_count;
  result.sample_rate = kProcessorAudioSampleRate;
  result.channel_count = kProcessorAudioChannelCount;
  result.pts = pts;
  result.time_base = time_base;
  result.pts_us = pts_us;
  return result;
}

AudioConverter::AudioConverter() = default;

AudioConverter::~AudioConverter() = default;

std::optional<NormalizedAudioFrame> AudioConverter::Convert(
    const ffmpeg::Frame& input) {
  if (state_ != nullptr && state_->flushed) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot convert audio after flushing");
  }
  if (!is_configured()) {
    Configure(input);
  } else {
    ValidateInput(input);
  }

  AnchorTimeline(input);
  const AVFrame* frame = input.get();
  const auto* input_data =
      const_cast<const uint8_t* const*>(frame->extended_data);
  return ConvertSamples(input_data, frame->nb_samples);
}

std::optional<NormalizedAudioFrame> AudioConverter::Flush() {
  if (!is_configured() || state_->flushed) {
    return std::nullopt;
  }

  const int output_capacity = swr_get_out_samples(state_->context.get(), 0);
  CheckFfmpeg(output_capacity, "failed to query resampler flush capacity");
  if (output_capacity == 0) {
    state_->flushed = true;
    return std::nullopt;
  }

  NormalizedAudioFrame output;
  SetOutputTimestamp(&output);
  output.samples.resize(static_cast<std::size_t>(output_capacity) *
                        kProcessorAudioChannelCount);
  uint8_t* output_data[] = {reinterpret_cast<uint8_t*>(output.samples.data())};
  const int converted = swr_convert(state_->context.get(), output_data,
                                    output_capacity, nullptr, 0);
  CheckFfmpeg(converted, "failed to flush the audio resampler");
  if (converted == 0) {
    state_->flushed = true;
    return std::nullopt;
  }

  output.frame_count = static_cast<std::size_t>(converted);
  output.samples.resize(output.frame_count * kProcessorAudioChannelCount);
  state_->total_output_frames += converted;
  return output;
}

bool AudioConverter::is_configured() const noexcept {
  return state_ != nullptr && state_->context.get() != nullptr;
}

void AudioConverter::Configure(const ffmpeg::Frame& input) {
  const AVFrame* frame = input.get();
  if (frame == nullptr || frame->nb_samples <= 0 ||
      frame->extended_data == nullptr || frame->sample_rate <= 0 ||
      frame->format == AV_SAMPLE_FMT_NONE ||
      av_channel_layout_check(&frame->ch_layout) == 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "decoded audio frame has invalid format information");
  }

  auto candidate_state = std::make_unique<State>(frame->ch_layout);
  ffmpeg::ChannelLayout output_layout(kProcessorAudioChannelCount);
  const auto input_format = static_cast<AVSampleFormat>(frame->format);
  CheckFfmpeg(
      swr_alloc_set_opts2(candidate_state->context.inout(), output_layout.get(),
                          AV_SAMPLE_FMT_FLT, kProcessorAudioSampleRate,
                          candidate_state->source_channel_layout.get(),
                          input_format, frame->sample_rate, 0, nullptr),
      "failed to configure the audio resampler");
  CheckFfmpeg(swr_init(candidate_state->context.get()),
              "failed to initialize the audio resampler");

  candidate_state->source_sample_format = input_format;
  candidate_state->source_sample_rate = frame->sample_rate;
  candidate_state->source_time_base = frame->time_base;
  state_ = std::move(candidate_state);
}

void AudioConverter::ValidateInput(const ffmpeg::Frame& input) const {
  const AVFrame* frame = input.get();
  if (frame == nullptr || frame->nb_samples <= 0 ||
      frame->extended_data == nullptr || frame->sample_rate <= 0 ||
      frame->format == AV_SAMPLE_FMT_NONE ||
      av_channel_layout_check(&frame->ch_layout) == 0) {
    ThrowError(StatusCode::kInvalidArgument, "decoded audio frame is invalid");
  }

  const bool layout_matches =
      av_channel_layout_compare(state_->source_channel_layout.get(),
                                &frame->ch_layout) == 0;
  if (frame->sample_rate != state_->source_sample_rate ||
      frame->format != state_->source_sample_format || !layout_matches) {
    ThrowError(StatusCode::kUnsupported,
               "input audio format changed while converting");
  }
}

std::optional<NormalizedAudioFrame> AudioConverter::ConvertSamples(
    const uint8_t* const* input_data, int input_frame_count) {
  const int output_capacity =
      swr_get_out_samples(state_->context.get(), input_frame_count);
  CheckFfmpeg(output_capacity, "failed to query resampler output capacity");
  if (output_capacity == 0) {
    return std::nullopt;
  }

  NormalizedAudioFrame output;
  SetOutputTimestamp(&output);
  output.samples.resize(static_cast<std::size_t>(output_capacity) *
                        kProcessorAudioChannelCount);
  uint8_t* output_data[] = {reinterpret_cast<uint8_t*>(output.samples.data())};
  const int converted =
      swr_convert(state_->context.get(), output_data, output_capacity,
                  input_data, input_frame_count);
  CheckFfmpeg(converted, "failed to convert decoded audio");
  if (converted == 0) {
    return std::nullopt;
  }

  output.frame_count = static_cast<std::size_t>(converted);
  output.samples.resize(output.frame_count * kProcessorAudioChannelCount);
  state_->total_output_frames += converted;
  return output;
}

void AudioConverter::SetOutputTimestamp(NormalizedAudioFrame* output) const {
  output->time_base.numerator = state_->source_time_base.num;
  output->time_base.denominator = state_->source_time_base.den;
  if (state_->source_base_pts == MW_TIMESTAMP_UNAVAILABLE ||
      state_->source_base_pts_us == MW_TIMESTAMP_UNAVAILABLE) {
    output->pts = MW_TIMESTAMP_UNAVAILABLE;
    output->pts_us = MW_TIMESTAMP_UNAVAILABLE;
    return;
  }

  const int64_t output_offset =
      state_->total_output_frames - state_->source_base_output_frame;
  output->pts =
      AddTimestampOffset(state_->source_base_pts, state_->source_time_base,
                         output_offset, kOutputTimeBase);
  output->pts_us =
      AddTimestampOffset(state_->source_base_pts_us, kMicrosecondsTimeBase,
                         output_offset, kOutputTimeBase);
}

void AudioConverter::AnchorTimeline(const ffmpeg::Frame& input) {
  const AVFrame* frame = input.get();
  if (state_->source_base_pts != MW_TIMESTAMP_UNAVAILABLE ||
      frame->pts == AV_NOPTS_VALUE || !IsValidTimeBase(frame->time_base)) {
    return;
  }

  state_->source_base_pts = frame->pts;
  state_->source_base_pts_us = ToMicroseconds(frame->pts, frame->time_base);
  state_->source_base_output_frame = state_->total_output_frames;
  state_->source_time_base = frame->time_base;
}

}  // namespace mw::streamer::internal

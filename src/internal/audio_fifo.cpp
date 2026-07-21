#include "mw/streamer/internal/audio_fifo.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kOutputTimeBase{1, kProcessorAudioSampleRate};

int64_t FramePts(const NormalizedAudioFrame& frame) {
  const AVRational time_base{frame.time_base.numerator,
                             frame.time_base.denominator};
  if (frame.pts != MW_TIMESTAMP_UNAVAILABLE && IsValidTimeBase(time_base)) {
    return RescaleTimestamp(frame.pts, time_base, kOutputTimeBase);
  }
  if (frame.pts_us != MW_TIMESTAMP_UNAVAILABLE) {
    return RescaleTimestamp(frame.pts_us, kMicrosecondsTimeBase,
                            kOutputTimeBase);
  }
  return MW_TIMESTAMP_UNAVAILABLE;
}

}  // namespace

AudioFifo::AudioFifo(std::size_t block_frame_count,
                     std::size_t max_buffered_frames)
    : block_frame_count_(block_frame_count),
      max_buffered_frames_(max_buffered_frames) {}

void AudioFifo::Push(NormalizedAudioFrame frame) {
  if (block_frame_count_ == 0 || max_buffered_frames_ < block_frame_count_) {
    ThrowError(StatusCode::kInvalidArgument,
               "AudioFifo has invalid capacity configuration");
  }
  if (flushing_) {
    ThrowError(StatusCode::kInvalidArgument,
               "cannot push audio after FIFO flushing begins");
  }
  if (frame.samples.size() != frame.frame_count * kProcessorAudioChannelCount) {
    ThrowError(StatusCode::kInvalidArgument,
               "normalized audio frame has an invalid sample count");
  }
  if (frame.frame_count == 0) {
    return;
  }

  int64_t frame_pts = FramePts(frame);
  if (expected_next_frame_pts_ != MW_TIMESTAMP_UNAVAILABLE &&
      frame_pts != MW_TIMESTAMP_UNAVAILABLE &&
      frame_pts < expected_next_frame_pts_) {
    const std::size_t overlap_frames =
        static_cast<std::size_t>(expected_next_frame_pts_ - frame_pts);
    if (overlap_frames >= frame.frame_count) {
      return;
    }
    frame.samples.erase(frame.samples.begin(),
                        frame.samples.begin() +
                            static_cast<std::ptrdiff_t>(
                                overlap_frames * kProcessorAudioChannelCount));
    frame.frame_count -= overlap_frames;
    const AVRational frame_time_base{frame.time_base.numerator,
                                     frame.time_base.denominator};
    if (frame.pts != MW_TIMESTAMP_UNAVAILABLE &&
        IsValidTimeBase(frame_time_base)) {
      frame.pts = AddTimestampOffset(frame.pts, frame_time_base,
                                     static_cast<int64_t>(overlap_frames),
                                     kOutputTimeBase);
    }
    if (frame.pts_us != MW_TIMESTAMP_UNAVAILABLE) {
      frame.pts_us = AddTimestampOffset(frame.pts_us, kMicrosecondsTimeBase,
                                        static_cast<int64_t>(overlap_frames),
                                        kOutputTimeBase);
    }
    frame_pts += static_cast<int64_t>(overlap_frames);
  }

  if (frame.frame_count > max_buffered_frames_ - buffered_frame_count_) {
    ThrowError(StatusCode::kUnavailable,
               "AudioFifo buffered frame capacity exceeded");
  }

  if (frame_pts != MW_TIMESTAMP_UNAVAILABLE) {
    expected_next_frame_pts_ =
        frame_pts + static_cast<int64_t>(frame.frame_count);
  }
  buffered_frame_count_ += frame.frame_count;
  Segment segment;
  segment.frame = std::move(frame);
  segments_.push_back(std::move(segment));
}

std::optional<NormalizedAudioFrame> AudioFifo::Read() {
  if (flushing_) {
    ThrowError(StatusCode::kInvalidArgument,
               "use AudioFifo::Flush after flushing begins");
  }
  if (buffered_frame_count_ < block_frame_count_) {
    return std::nullopt;
  }
  return ReadBlock(false);
}

std::optional<NormalizedAudioFrame> AudioFifo::Flush() {
  flushing_ = true;
  if (buffered_frame_count_ == 0) {
    return std::nullopt;
  }
  return ReadBlock(true);
}

std::size_t AudioFifo::buffered_frame_count() const noexcept {
  return buffered_frame_count_;
}

NormalizedAudioFrame AudioFifo::ReadBlock(bool pad_with_silence) {
  if (segments_.empty() ||
      (!pad_with_silence && buffered_frame_count_ < block_frame_count_)) {
    ThrowError(StatusCode::kInternal,
               "AudioFifo does not contain a complete block");
  }

  NormalizedAudioFrame output;
  output.frame_count = block_frame_count_;
  output.samples.assign(block_frame_count_ * kProcessorAudioChannelCount, 0.0F);
  SetOutputTimestamp(segments_.front(), &output);

  std::size_t copied_frames = 0;
  while (copied_frames < block_frame_count_ && !segments_.empty()) {
    Segment& segment = segments_.front();
    const std::size_t available_frames =
        segment.frame.frame_count - segment.consumed_frame_count;
    const std::size_t copy_frames =
        std::min(block_frame_count_ - copied_frames, available_frames);
    const auto source_begin =
        segment.frame.samples.begin() +
        static_cast<std::ptrdiff_t>(segment.consumed_frame_count *
                                    kProcessorAudioChannelCount);
    std::copy_n(source_begin, copy_frames * kProcessorAudioChannelCount,
                output.samples.begin() +
                    static_cast<std::ptrdiff_t>(copied_frames *
                                                kProcessorAudioChannelCount));

    copied_frames += copy_frames;
    segment.consumed_frame_count += copy_frames;
    buffered_frame_count_ -= copy_frames;
    if (segment.consumed_frame_count == segment.frame.frame_count) {
      segments_.pop_front();
    }
  }
  return output;
}

void AudioFifo::SetOutputTimestamp(const Segment& segment,
                                   NormalizedAudioFrame* output) const {
  output->time_base = segment.frame.time_base;
  const int64_t consumed = static_cast<int64_t>(segment.consumed_frame_count);
  const AVRational frame_time_base{segment.frame.time_base.numerator,
                                   segment.frame.time_base.denominator};
  output->pts = AddTimestampOffset(segment.frame.pts, frame_time_base, consumed,
                                   kOutputTimeBase);
  output->pts_us = AddTimestampOffset(
      segment.frame.pts_us, kMicrosecondsTimeBase, consumed, kOutputTimeBase);
}

}  // namespace mw::streamer::internal

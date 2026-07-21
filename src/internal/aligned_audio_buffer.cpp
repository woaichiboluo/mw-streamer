#include "mw/streamer/internal/aligned_audio_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kAudioTimeBase{1, kProcessorAudioSampleRate};

int64_t ToAudioFramePts(const NormalizedAudioFrame& frame) {
  const AVRational source_time_base{frame.time_base.numerator,
                                    frame.time_base.denominator};
  if (frame.pts == MW_TIMESTAMP_UNAVAILABLE ||
      !IsValidTimeBase(source_time_base)) {
    ThrowError(StatusCode::kUnsupported,
               "cannot align an audio frame without a valid original PTS");
  }
  return RescaleTimestamp(frame.pts, source_time_base, kAudioTimeBase);
}

int64_t CheckedEndPts(int64_t start_pts, std::size_t frame_count) {
  if (frame_count >
          static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) ||
      start_pts > std::numeric_limits<int64_t>::max() -
                      static_cast<int64_t>(frame_count)) {
    ThrowError(StatusCode::kInvalidArgument,
               "aligned audio interval exceeds the timestamp range");
  }
  return start_pts + static_cast<int64_t>(frame_count);
}

}  // namespace

AlignedAudioBuffer::AlignedAudioBuffer(std::size_t max_buffered_frames)
    : max_buffered_frames_(max_buffered_frames) {
  if (max_buffered_frames_ == 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "aligned audio buffer capacity must be positive");
  }
}

void AlignedAudioBuffer::Push(NormalizedAudioFrame frame) {
  PushImpl(std::move(frame), false);
}

void AlignedAudioBuffer::PushDroppingOldest(NormalizedAudioFrame frame) {
  PushImpl(std::move(frame), true);
}

void AlignedAudioBuffer::PushImpl(NormalizedAudioFrame frame,
                                  bool drop_oldest) {
  if (frame.samples.size() % kProcessorAudioChannelCount != 0 ||
      frame.samples.size() / kProcessorAudioChannelCount != frame.frame_count) {
    ThrowError(StatusCode::kInvalidArgument,
               "normalized audio frame has an invalid sample count");
  }
  if (frame.frame_count == 0) {
    return;
  }

  const int64_t frame_start_pts = ToAudioFramePts(frame);
  const int64_t frame_end_pts =
      CheckedEndPts(frame_start_pts, frame.frame_count);
  int64_t retained_start_pts = frame_start_pts;
  if (accepted_until_pts_ != MW_TIMESTAMP_UNAVAILABLE) {
    retained_start_pts = std::max(retained_start_pts, accepted_until_pts_);
  }
  if (read_until_pts_ != MW_TIMESTAMP_UNAVAILABLE) {
    retained_start_pts = std::max(retained_start_pts, read_until_pts_);
  }
  if (retained_start_pts >= frame_end_pts) {
    return;
  }

  std::size_t cropped_frame_count =
      static_cast<std::size_t>(retained_start_pts - frame_start_pts);
  std::size_t retained_frame_count = frame.frame_count - cropped_frame_count;
  if (drop_oldest && retained_frame_count > max_buffered_frames_) {
    const std::size_t additional_crop =
        retained_frame_count - max_buffered_frames_;
    cropped_frame_count += additional_crop;
    retained_frame_count = max_buffered_frames_;
  }
  if (drop_oldest && retained_frame_count > remaining_capacity_frames()) {
    DiscardOldest(retained_frame_count - remaining_capacity_frames());
  }
  if (retained_frame_count > max_buffered_frames_ - buffered_frame_count_) {
    ThrowError(StatusCode::kUnavailable,
               "aligned audio buffered frame capacity exceeded");
  }

  Segment segment;
  segment.frame = std::move(frame);
  segment.start_pts = frame_start_pts;
  segment.consumed_frame_count = cropped_frame_count;
  segments_.push_back(std::move(segment));
  buffered_frame_count_ += retained_frame_count;
  accepted_until_pts_ = frame_end_pts;
}

void AlignedAudioBuffer::DiscardOldest(std::size_t frame_count) noexcept {
  while (frame_count > 0 && !segments_.empty()) {
    Segment& segment = segments_.front();
    const std::size_t available_frame_count =
        segment.frame.frame_count - segment.consumed_frame_count;
    const std::size_t discarded_frame_count =
        std::min(frame_count, available_frame_count);
    segment.consumed_frame_count += discarded_frame_count;
    buffered_frame_count_ -= discarded_frame_count;
    frame_count -= discarded_frame_count;
    if (segment.consumed_frame_count == segment.frame.frame_count) {
      segments_.pop_front();
    }
  }
}

NormalizedAudioFrame AlignedAudioBuffer::Read(int64_t start_pts,
                                              std::size_t frame_count) {
  if (start_pts == MW_TIMESTAMP_UNAVAILABLE || frame_count == 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "aligned audio read interval is invalid");
  }
  if (read_until_pts_ != MW_TIMESTAMP_UNAVAILABLE &&
      start_pts < read_until_pts_) {
    ThrowError(
        StatusCode::kInvalidArgument,
        "aligned audio read intervals must not overlap or move backward");
  }
  const int64_t end_pts = CheckedEndPts(start_pts, frame_count);

  NormalizedAudioFrame output;
  output.samples.assign(frame_count * kProcessorAudioChannelCount, 0.0F);
  output.frame_count = frame_count;
  output.pts = start_pts;
  output.time_base = {kAudioTimeBase.num, kAudioTimeBase.den};
  output.pts_us = ToMicroseconds(start_pts, kAudioTimeBase);

  while (!segments_.empty()) {
    Segment& segment = segments_.front();
    const std::size_t available_frame_count =
        segment.frame.frame_count - segment.consumed_frame_count;
    const int64_t available_start_pts =
        segment.start_pts + static_cast<int64_t>(segment.consumed_frame_count);
    const int64_t available_end_pts =
        available_start_pts + static_cast<int64_t>(available_frame_count);

    if (available_start_pts >= end_pts) {
      break;
    }
    if (available_end_pts <= start_pts) {
      buffered_frame_count_ -= available_frame_count;
      segments_.pop_front();
      continue;
    }

    if (available_start_pts < start_pts) {
      const std::size_t discarded_frame_count =
          static_cast<std::size_t>(start_pts - available_start_pts);
      segment.consumed_frame_count += discarded_frame_count;
      buffered_frame_count_ -= discarded_frame_count;
    }

    const int64_t copy_start_pts =
        segment.start_pts + static_cast<int64_t>(segment.consumed_frame_count);
    const std::size_t remaining_frame_count =
        segment.frame.frame_count - segment.consumed_frame_count;
    const std::size_t copy_frame_count =
        std::min(remaining_frame_count,
                 static_cast<std::size_t>(end_pts - copy_start_pts));
    const auto source_begin =
        segment.frame.samples.begin() +
        static_cast<std::ptrdiff_t>(segment.consumed_frame_count *
                                    kProcessorAudioChannelCount);
    const auto output_begin =
        output.samples.begin() +
        static_cast<std::ptrdiff_t>((copy_start_pts - start_pts) *
                                    kProcessorAudioChannelCount);
    std::copy_n(source_begin, copy_frame_count * kProcessorAudioChannelCount,
                output_begin);
    segment.consumed_frame_count += copy_frame_count;
    buffered_frame_count_ -= copy_frame_count;
    if (segment.consumed_frame_count == segment.frame.frame_count) {
      segments_.pop_front();
    }
  }

  read_until_pts_ = end_pts;
  return output;
}

std::size_t AlignedAudioBuffer::buffered_frame_count() const noexcept {
  return buffered_frame_count_;
}

std::size_t AlignedAudioBuffer::remaining_capacity_frames() const noexcept {
  return max_buffered_frames_ - buffered_frame_count_;
}

std::optional<int64_t> AlignedAudioBuffer::accepted_until_pts() const noexcept {
  if (accepted_until_pts_ == MW_TIMESTAMP_UNAVAILABLE) {
    return std::nullopt;
  }
  return accepted_until_pts_;
}

}  // namespace mw::streamer::internal

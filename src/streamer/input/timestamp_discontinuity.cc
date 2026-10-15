#include "timestamp_discontinuity.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/log.h"

namespace mw::streamer::internal {
namespace {

constexpr AVRational kMicroseconds{1, AV_TIME_BASE};
constexpr std::int64_t kJumpThresholdUs = std::int64_t{10} * AV_TIME_BASE;
constexpr std::int64_t kBackwardToleranceUs = AV_TIME_BASE / 10;
constexpr std::int64_t kErrorThresholdUs =
    std::int64_t{30} * 60 * 60 * AV_TIME_BASE;
constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

std::int64_t Add(std::int64_t value, std::int64_t offset) {
  if ((offset > 0 && value > kMax - offset) ||
      (offset < 0 && value <= kMin - offset))
    throw std::overflow_error("输入时间戳相加超出范围");
  return value + offset;
}

std::int64_t Subtract(std::int64_t value, std::int64_t offset) {
  if ((offset > 0 && value <= kMin + offset) ||
      (offset < 0 && value > kMax + offset))
    throw std::overflow_error("输入时间戳相减超出范围");
  return value - offset;
}

std::int64_t Rescale(std::int64_t value, AVRational from, AVRational to) {
  const auto result = av_rescale_q(value, from, to);
  if (result == AV_NOPTS_VALUE)
    throw std::overflow_error("输入时间戳换算超出范围");
  return result;
}

bool Exceeds(std::int64_t delta, std::int64_t threshold) {
  return delta > threshold || delta < -threshold;
}

void Shift(ffmpeg::Packet& packet, std::int64_t offset) {
  if (packet->dts != AV_NOPTS_VALUE) packet->dts = Add(packet->dts, offset);
  if (packet->pts != AV_NOPTS_VALUE) packet->pts = Add(packet->pts, offset);
}

}  // namespace

TimestampDiscontinuity::TimestampDiscontinuity(const AvInputInfo& info)
    : discontinuous_(info.timestamp_discontinuities) {
  for (const auto& stream : info.streams) {
    const auto* parameters = stream.codec_parameters.get();
    auto frame_rate = parameters->framerate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0)
      frame_rate = stream.frame_rate;
    tracks_.push_back({stream.stream_index, parameters->codec_type,
                       stream.time_base, frame_rate, parameters->sample_rate,
                       parameters->frame_size, parameters->video_delay});
  }
}

void TimestampDiscontinuity::Correct(ffmpeg::Packet& packet, const Track& track,
                                     std::int64_t delta_us) {
  offset_us_ = Subtract(offset_us_, delta_us);
  const auto delta = Rescale(delta_us, kMicroseconds, track.time_base);
  if (packet->dts != AV_NOPTS_VALUE) packet->dts = Subtract(packet->dts, delta);
  if (packet->pts != AV_NOPTS_VALUE) packet->pts = Subtract(packet->pts, delta);
  MW_LOG_WARNING("streamer",
                 "输入时间轴跳变已修正: stream={} delta_us={} offset_us={}",
                 track.index, delta_us, offset_us_);
}

void TimestampDiscontinuity::Detect(ffmpeg::Packet& packet,
                                    const Track& track) {
  const auto dts_us = Rescale(packet->dts, track.time_base, kMicroseconds);
  if (track.next_dts_us != AV_NOPTS_VALUE) {
    const auto delta = Subtract(dts_us, track.next_dts_us);
    if (discontinuous_) {
      if (Exceeds(delta, kJumpThresholdUs) ||
          (dts_us < track.dts_us &&
           Subtract(track.dts_us, dts_us) > kBackwardToleranceUs))
        Correct(packet, track, delta);
    } else {
      if (Exceeds(delta, kErrorThresholdUs)) {
        MW_LOG_WARNING("streamer", "丢弃异常输入 DTS: stream={} dts={}",
                       track.index, packet->dts);
        packet->dts = AV_NOPTS_VALUE;
      }
      if (packet->pts != AV_NOPTS_VALUE &&
          Exceeds(Subtract(Rescale(packet->pts, track.time_base, kMicroseconds),
                           track.next_dts_us),
                  kErrorThresholdUs)) {
        MW_LOG_WARNING("streamer", "丢弃异常输入 PTS: stream={} pts={}",
                       track.index, packet->pts);
        packet->pts = AV_NOPTS_VALUE;
      }
    }
  } else if (discontinuous_ && last_dts_us_ != AV_NOPTS_VALUE) {
    const auto delta = Subtract(dts_us, last_dts_us_);
    if (Exceeds(delta, kJumpThresholdUs)) Correct(packet, track, delta);
  }
  last_dts_us_ = packet->dts == AV_NOPTS_VALUE
                     ? AV_NOPTS_VALUE
                     : Rescale(packet->dts, track.time_base, kMicroseconds);
}

void TimestampDiscontinuity::Predict(const ffmpeg::Packet& packet,
                                     Track& track) {
  if (!track.saw_first_timestamp) {
    track.dts_us = 0;
    if (track.type == AVMEDIA_TYPE_VIDEO && track.frame_rate.num > 0 &&
        track.frame_rate.den > 0) {
      const auto delay =
          av_rescale_q_rnd(track.video_delay, av_inv_q(track.frame_rate),
                           kMicroseconds, AV_ROUND_ZERO);
      track.dts_us = Subtract(0, delay);
    }
    if (packet->pts != AV_NOPTS_VALUE)
      track.dts_us = Add(track.dts_us,
                         Rescale(packet->pts, track.time_base, kMicroseconds));
    track.saw_first_timestamp = true;
  }
  if (track.next_dts_us == AV_NOPTS_VALUE) track.next_dts_us = track.dts_us;
  if (packet->dts != AV_NOPTS_VALUE)
    track.next_dts_us = Rescale(packet->dts, track.time_base, kMicroseconds);
  track.dts_us = track.next_dts_us;
  std::int64_t duration_us = 0;
  if (track.type == AVMEDIA_TYPE_AUDIO && track.sample_rate > 0) {
    // Match ist_dts_update: codec frame size takes precedence, even when zero.
    duration_us = av_rescale_rnd(track.frame_size, AV_TIME_BASE,
                                 track.sample_rate, AV_ROUND_ZERO);
  } else if (packet->duration != 0) {
    duration_us = Rescale(packet->duration, track.time_base, kMicroseconds);
  } else if (track.type == AVMEDIA_TYPE_VIDEO && track.frame_rate.num > 0 &&
             track.frame_rate.den > 0) {
    duration_us = Rescale(1, av_inv_q(track.frame_rate), kMicroseconds);
  }
  track.next_dts_us = Add(track.next_dts_us, duration_us);
}

void TimestampDiscontinuity::Process(ffmpeg::Packet& packet) {
  const auto found = std::find_if(
      tracks_.begin(), tracks_.end(),
      [&](const Track& track) { return track.index == packet->stream_index; });
  if (found == tracks_.end())
    throw std::invalid_argument("输入时间戳修正缺少轨道描述");
  auto& track = *found;
  Shift(packet, Rescale(offset_us_, kMicroseconds, track.time_base));
  if (track.type != AVMEDIA_TYPE_AUDIO && track.type != AVMEDIA_TYPE_VIDEO)
    return;
  if (packet->dts != AV_NOPTS_VALUE) Detect(packet, track);
  Predict(packet, track);
}

}  // namespace mw::streamer::internal

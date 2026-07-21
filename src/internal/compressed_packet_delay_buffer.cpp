#include "mw/streamer/internal/compressed_packet_delay_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"

namespace mw::streamer::internal {

CompressedPacketDelayBuffer::CompressedPacketDelayBuffer(
    std::size_t input_count, std::chrono::milliseconds delay)
    : delay_(std::chrono::duration_cast<std::chrono::microseconds>(delay)),
      inputs_(input_count) {
  if (input_count == 0 || delay.count() < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "compressed packet delay buffer configuration is invalid");
  }
}

bool CompressedPacketDelayBuffer::Push(ScheduledPacket packet) {
  const AVPacket* raw_packet = packet.packet.get();
  if (packet.input_index >= inputs_.size() || raw_packet == nullptr ||
      raw_packet->dts == AV_NOPTS_VALUE ||
      !IsValidTimeBase(raw_packet->time_base)) {
    ThrowError(StatusCode::kInvalidArgument,
               "delayed compressed packet requires a valid DTS and time base");
  }
  const std::int64_t source_dts_us =
      ToMicroseconds(raw_packet->dts, raw_packet->time_base);
  if (packet.source_dts_us != source_dts_us) {
    ThrowError(StatusCode::kInvalidArgument,
               "delayed packet source DTS does not match the AVPacket");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (cancelled_) {
    return false;
  }
  InputState& input = inputs_[packet.input_index];
  input.first_timeline_dts_us =
      input.first_timeline_dts_us.has_value()
          ? std::min(*input.first_timeline_dts_us, packet.timeline_dts_us)
          : packet.timeline_dts_us;
  input.maximum_timeline_dts_us =
      input.maximum_timeline_dts_us.has_value()
          ? std::max(*input.maximum_timeline_dts_us, packet.timeline_dts_us)
          : packet.timeline_dts_us;
  input.last_timeline_dts_us = input.maximum_timeline_dts_us;
  const auto key = std::make_pair(packet.timeline_dts_us, packet.sequence);
  if (!input.packets.emplace(key, std::move(packet)).second) {
    ThrowError(StatusCode::kInvalidArgument,
               "delayed packet sequence must be unique within an input");
  }
  TryStartLocked();
  changed_.notify_all();
  return true;
}

std::optional<ScheduledPacket> CompressedPacketDelayBuffer::WaitPop(
    std::size_t input_index) {
  if (input_index >= inputs_.size()) {
    ThrowError(StatusCode::kInvalidArgument,
               "delayed packet input index is out of range");
  }

  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    changed_.wait(lock, [this, input_index] {
      return cancelled_ || (steady_origin_.has_value() &&
                            !inputs_[input_index].packets.empty());
    });
    if (cancelled_) {
      return std::nullopt;
    }

    InputState& input = inputs_[input_index];
    auto first = input.packets.begin();
    const auto release_time =
        *steady_origin_ + std::chrono::microseconds(
                              first->second.timeline_dts_us - media_origin_us_);
    if (changed_.wait_until(lock, release_time,
                            [this] { return cancelled_; })) {
      return std::nullopt;
    }
    ScheduledPacket result = std::move(first->second);
    input.packets.erase(first);
    return result;
  }
}

void CompressedPacketDelayBuffer::Cancel() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  cancelled_ = true;
  for (InputState& input : inputs_) {
    input.packets.clear();
  }
  changed_.notify_all();
}

bool CompressedPacketDelayBuffer::is_started() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return steady_origin_.has_value();
}

std::optional<std::int64_t> CompressedPacketDelayBuffer::playback_cursor_us()
    const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!steady_origin_.has_value()) {
    return std::nullopt;
  }
  return media_origin_us_ +
         std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - *steady_origin_)
             .count();
}

std::optional<std::int64_t> CompressedPacketDelayBuffer::last_timeline_dts_us(
    std::size_t input_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (input_index >= inputs_.size()) {
    ThrowError(StatusCode::kInvalidArgument,
               "delayed packet input index is out of range");
  }
  return inputs_[input_index].last_timeline_dts_us;
}

void CompressedPacketDelayBuffer::TryStartLocked() {
  if (steady_origin_.has_value() ||
      !std::all_of(
          inputs_.begin(), inputs_.end(),
          [this](const InputState& input) { return IsWarmedLocked(input); })) {
    return;
  }

  media_origin_us_ = inputs_.front().packets.begin()->second.timeline_dts_us;
  for (const InputState& input : inputs_) {
    media_origin_us_ = std::min(media_origin_us_,
                                input.packets.begin()->second.timeline_dts_us);
  }
  steady_origin_ = std::chrono::steady_clock::now();
}

bool CompressedPacketDelayBuffer::IsWarmedLocked(
    const InputState& input) const noexcept {
  return input.first_timeline_dts_us.has_value() &&
         input.maximum_timeline_dts_us.has_value() &&
         *input.maximum_timeline_dts_us - *input.first_timeline_dts_us >=
             delay_.count();
}

}  // namespace mw::streamer::internal

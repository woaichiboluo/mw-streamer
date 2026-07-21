#include "mw/streamer/internal/video_synchronizer.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/streamer/internal/common/error.h"

namespace mw::streamer::internal {
namespace {

std::uint64_t TimestampDistance(std::int64_t left,
                                std::int64_t right) noexcept {
  if (left >= right) {
    return static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right);
  }
  return static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}

}  // namespace

VideoSynchronizer::VideoSynchronizer(VideoSynchronizerConfig config)
    : config_(config), queues_(config.input_count) {
  if (config_.input_count == 0 ||
      config_.primary_input_index >= config_.input_count ||
      config_.pts_tolerance.count() < 0 || config_.maximum_wait.count() <= 0 ||
      config_.queue_capacity == 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "video synchronizer configuration is invalid");
  }
}

void VideoSynchronizer::Push(std::size_t input_index, ffmpeg::Frame frame,
                             std::int64_t timeline_pts_us, TimePoint now) {
  if (input_index >= config_.input_count) {
    ThrowError(StatusCode::kInvalidArgument,
               "video synchronizer input index is out of range");
  }
  if (frame.get() == nullptr) {
    ThrowError(StatusCode::kInvalidArgument,
               "video synchronizer requires a valid frame");
  }
  if (timeline_pts_us == AV_NOPTS_VALUE) {
    ThrowError(StatusCode::kInvalidArgument,
               "video synchronizer requires a valid timeline PTS");
  }

  Queue* queue = &queues_[input_index];
  const auto position =
      std::upper_bound(queue->begin(), queue->end(), timeline_pts_us,
                       [](std::int64_t value, const QueuedFrame& queued) {
                         return value < queued.timeline_pts_us;
                       });
  queue->insert(position, QueuedFrame{std::move(frame), timeline_pts_us, now});
  if (queue->size() > config_.queue_capacity) {
    queue->pop_front();
  }
}

std::optional<SynchronizedVideoFrames> VideoSynchronizer::Pop(TimePoint now) {
  Queue* primary_queue = &queues_[config_.primary_input_index];
  const std::uint64_t tolerance =
      static_cast<std::uint64_t>(config_.pts_tolerance.count());

  while (!primary_queue->empty()) {
    QueuedFrame& primary = primary_queue->front();
    DiscardFramesTooOldFor(primary.timeline_pts_us);
    const bool timed_out = now - primary.arrival_time >= config_.maximum_wait;
    bool must_wait = false;
    bool cannot_match = false;

    for (std::size_t input_index = 0; input_index < config_.input_count;
         ++input_index) {
      if (input_index == config_.primary_input_index) {
        continue;
      }

      Queue* queue = &queues_[input_index];
      const auto closest = FindClosest(queue, primary.timeline_pts_us);
      const bool has_candidate =
          closest != queue->end() &&
          TimestampDistance(closest->timeline_pts_us,
                            primary.timeline_pts_us) <= tolerance;
      const bool reached_primary =
          !queue->empty() &&
          queue->back().timeline_pts_us >= primary.timeline_pts_us;

      if (!has_candidate && (timed_out || reached_primary)) {
        cannot_match = true;
        break;
      }
      if (!timed_out && (!has_candidate || !reached_primary)) {
        must_wait = true;
      }
    }

    if (cannot_match) {
      primary_queue->pop_front();
      continue;
    }
    if (must_wait) {
      return std::nullopt;
    }

    SynchronizedVideoFrames result;
    result.primary_pts_us = primary.timeline_pts_us;
    result.frames.reserve(config_.input_count);
    for (std::size_t input_index = 0; input_index < config_.input_count;
         ++input_index) {
      Queue* queue = &queues_[input_index];
      if (input_index == config_.primary_input_index) {
        result.frames.push_back(std::move(primary.frame));
        continue;
      }

      const auto closest = FindClosest(queue, primary.timeline_pts_us);
      result.frames.push_back(std::move(closest->frame));
      queue->erase(queue->begin(), std::next(closest));
    }
    primary_queue->pop_front();
    return result;
  }
  return std::nullopt;
}

std::size_t VideoSynchronizer::queued_frame_count(
    std::size_t input_index) const {
  if (input_index >= config_.input_count) {
    ThrowError(StatusCode::kInvalidArgument,
               "video synchronizer input index is out of range");
  }
  return queues_[input_index].size();
}

VideoSynchronizer::Queue::iterator VideoSynchronizer::FindClosest(
    Queue* queue, std::int64_t pts_us) {
  auto later =
      std::lower_bound(queue->begin(), queue->end(), pts_us,
                       [](const QueuedFrame& queued, std::int64_t value) {
                         return queued.timeline_pts_us < value;
                       });
  if (later == queue->begin()) {
    return later;
  }
  if (later == queue->end()) {
    return std::prev(later);
  }

  auto earlier = std::prev(later);
  if (TimestampDistance(earlier->timeline_pts_us, pts_us) <=
      TimestampDistance(later->timeline_pts_us, pts_us)) {
    return earlier;
  }
  return later;
}

void VideoSynchronizer::DiscardFramesTooOldFor(std::int64_t primary_pts_us) {
  const std::uint64_t tolerance =
      static_cast<std::uint64_t>(config_.pts_tolerance.count());
  for (std::size_t input_index = 0; input_index < config_.input_count;
       ++input_index) {
    if (input_index == config_.primary_input_index) {
      continue;
    }

    Queue* queue = &queues_[input_index];
    while (!queue->empty() && queue->front().timeline_pts_us < primary_pts_us &&
           TimestampDistance(queue->front().timeline_pts_us, primary_pts_us) >
               tolerance) {
      queue->pop_front();
    }
  }
}

}  // namespace mw::streamer::internal

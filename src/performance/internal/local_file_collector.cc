#include "mw/performance/internal/local_file_collector.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace mw::streamer::performance::internal {
namespace {

VideoStageSnapshot MakeVideoStage(StageInterval interval,
                                  double seconds) noexcept {
  VideoStageSnapshot snapshot;
  snapshot.frames = interval.units;
  snapshot.frames_per_second =
      seconds > 0.0 ? static_cast<double>(interval.units) / seconds : 0.0;
  snapshot.latency = interval.latency;
  return snapshot;
}

AudioStageSnapshot MakeAudioStage(StageInterval interval,
                                  double seconds) noexcept {
  AudioStageSnapshot snapshot;
  snapshot.samples = interval.units;
  snapshot.samples_per_second =
      seconds > 0.0 ? static_cast<double>(interval.units) / seconds : 0.0;
  snapshot.latency = interval.latency;
  return snapshot;
}

}  // namespace

void LocalFileCollector::Reset() {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  audio_.Reset();
  video_.Reset();
  has_audio_ = false;
  has_video_ = false;
  duration_available_ = false;
  duration_ = std::chrono::microseconds::zero();
  last_position_available_ = false;
  last_position_us_ = 0;
  audio_position_us_.store(0, std::memory_order_relaxed);
  video_position_us_.store(0, std::memory_order_relaxed);
  audio_position_available_.store(false, std::memory_order_relaxed);
  video_position_available_.store(false, std::memory_order_relaxed);
  completed_.store(false, std::memory_order_relaxed);
  last_collection_ = std::chrono::steady_clock::now();
}

void LocalFileCollector::Configure(bool has_audio, bool has_video,
                                   bool duration_available,
                                   std::chrono::microseconds duration) {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  has_audio_ = has_audio;
  has_video_ = has_video;
  duration_available_ = duration_available;
  duration_ = duration_available
                  ? std::max(duration, std::chrono::microseconds{1})
                  : std::chrono::microseconds::zero();
}

void LocalFileCollector::RecordAudioPosition(
    std::chrono::microseconds position) noexcept {
  RecordPosition(audio_position_us_, audio_position_available_, position);
}

void LocalFileCollector::RecordVideoPosition(
    std::chrono::microseconds position) noexcept {
  RecordPosition(video_position_us_, video_position_available_, position);
}

void LocalFileCollector::MarkCompleted() noexcept {
  completed_.store(true, std::memory_order_release);
}

LocalFilePipelineSnapshot LocalFileCollector::Collect() {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto interval = now - last_collection_;
  last_collection_ = now;
  const double seconds = std::chrono::duration<double>(interval).count();
  const bool completed = completed_.load(std::memory_order_acquire);
  const auto position = ProcessedPosition();

  LocalFilePipelineSnapshot snapshot;
  snapshot.interval =
      std::chrono::duration_cast<std::chrono::nanoseconds>(interval);
  snapshot.has_audio = has_audio_;
  snapshot.has_video = has_video_;
  snapshot.progress_available = duration_available_;
  snapshot.duration = duration_;

  if (position.available) {
    const auto current_position_us =
        std::max<std::int64_t>(position.microseconds, 0);
    const auto delta_us =
        last_position_available_
            ? std::max(current_position_us - last_position_us_, std::int64_t{0})
            : current_position_us;
    snapshot.processing_speed_available = true;
    snapshot.processing_speed =
        seconds > 0.0 ? static_cast<double>(delta_us) / 1'000'000.0 / seconds
                      : 0.0;
    last_position_available_ = true;
    last_position_us_ = current_position_us;
    snapshot.processed_position =
        std::chrono::microseconds{current_position_us};
  }

  if (duration_available_) {
    if (completed) {
      snapshot.processed_position = duration_;
      snapshot.progress = 1.0;
    } else {
      snapshot.processed_position =
          std::min(snapshot.processed_position, duration_);
      snapshot.progress =
          std::clamp(static_cast<double>(snapshot.processed_position.count()) /
                         static_cast<double>(duration_.count()),
                     0.0, 1.0);
    }
  }

  if (has_audio_) {
    snapshot.audio.decode = MakeAudioStage(audio_.decode().Collect(), seconds);
    snapshot.audio.process =
        MakeAudioStage(audio_.process().Collect(), seconds);
  }
  if (has_video_) {
    snapshot.video.decode = MakeVideoStage(video_.decode().Collect(), seconds);
    snapshot.video.process =
        MakeVideoStage(video_.process().Collect(), seconds);
  }
  return snapshot;
}

void LocalFileCollector::RecordPosition(
    std::atomic<std::int64_t>& destination, std::atomic_bool& available,
    std::chrono::microseconds position) noexcept {
  const auto value = std::max<std::int64_t>(position.count(), 0);
  auto current = destination.load(std::memory_order_relaxed);
  while (current < value && !destination.compare_exchange_weak(
                                current, value, std::memory_order_relaxed)) {
  }
  available.store(true, std::memory_order_release);
}

LocalFileCollector::Position LocalFileCollector::ProcessedPosition()
    const noexcept {
  const bool audio_available =
      audio_position_available_.load(std::memory_order_acquire);
  const bool video_available =
      video_position_available_.load(std::memory_order_acquire);
  if ((has_audio_ && !audio_available) || (has_video_ && !video_available)) {
    return {};
  }
  if (has_audio_ && has_video_) {
    return {true, std::min(audio_position_us_.load(std::memory_order_relaxed),
                           video_position_us_.load(std::memory_order_relaxed))};
  }
  if (has_audio_) {
    return {true, audio_position_us_.load(std::memory_order_relaxed)};
  }
  if (has_video_) {
    return {true, video_position_us_.load(std::memory_order_relaxed)};
  }
  return {};
}

}  // namespace mw::streamer::performance::internal

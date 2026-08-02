#include "mw/performance/internal/streaming_collector.h"

#include <chrono>
#include <cstddef>
#include <utility>

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

void StreamingCollector::Reset() {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  audio_.Reset();
  video_.Reset();
  last_collection_ = std::chrono::steady_clock::now();
}

StreamingPipelineSnapshot StreamingCollector::Collect(
    bool has_audio, bool has_video, std::size_t audio_queue_depth,
    std::size_t video_queue_depth, std::size_t output_queue_depth,
    NetworkInputSnapshot input, std::vector<NetworkOutputSnapshot> outputs) {
  std::lock_guard<std::mutex> lock(collection_mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto interval = now - last_collection_;
  last_collection_ = now;
  const double seconds = std::chrono::duration<double>(interval).count();

  StreamingPipelineSnapshot snapshot;
  snapshot.interval =
      std::chrono::duration_cast<std::chrono::nanoseconds>(interval);
  snapshot.input = input;
  snapshot.outputs = std::move(outputs);
  snapshot.has_audio = has_audio;
  snapshot.has_video = has_video;
  snapshot.output_queue_depth = output_queue_depth;

  if (has_audio) {
    snapshot.audio.decode = MakeAudioStage(audio_.decode().Collect(), seconds);
    snapshot.audio.process =
        MakeAudioStage(audio_.process().Collect(), seconds);
    snapshot.audio.encode = MakeAudioStage(audio_.encode().Collect(), seconds);
    snapshot.audio.dropped_packets = audio_.CollectDroppedPackets();
    snapshot.audio.queue_depth = audio_queue_depth;
  }

  if (has_video) {
    snapshot.video.decode = MakeVideoStage(video_.decode().Collect(), seconds);
    snapshot.video.process =
        MakeVideoStage(video_.process().Collect(), seconds);
    snapshot.video.encode = MakeVideoStage(video_.encode().Collect(), seconds);
    snapshot.video.dropped_packets = video_.CollectDroppedPackets();
    snapshot.video.queue_depth = video_queue_depth;
  }
  return snapshot;
}

}  // namespace mw::streamer::performance::internal

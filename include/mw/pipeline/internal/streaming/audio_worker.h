#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_WORKER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "mw/common/blocking_queue.h"
#include "mw/decoder/config.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/pipeline/internal/streaming/audio_processing_chain.h"
#include "mw/processor/processor.h"

namespace mw::streamer::common {
class Barrier;
class Thread;
}  // namespace mw::streamer::common

namespace mw::streamer::processor {
class StreamingProcessorHandler;
}

namespace mw::streamer::performance::internal {
class TrackRecorder;
}

namespace mw::streamer::pipeline::internal::streaming {

class OutputWorker;

class AudioWorker final {
 public:
  AudioWorker(
      ffmpeg::StreamInfo stream_info,
      decoder::AudioDecoderConfig decoder_config, std::size_t queue_capacity,
      processor::StreamingProcessorHandler& processor,
      common::Barrier& boundary_barrier, OutputWorker& output,
      performance::internal::TrackRecorder& performance,
      std::function<void(const char* worker, const char* error)> on_failed);
  ~AudioWorker();

  AudioWorker(const AudioWorker&) = delete;
  AudioWorker& operator=(const AudioWorker&) = delete;

  void Start();
  bool Input(const ffmpeg::Packet& packet);
  void Reset();
  void End(bool final_end);
  void RequestStop() noexcept;
  void Stop() noexcept;
  std::size_t queue_depth() const;

 private:
  enum class WorkType {
    kPacket,
    kTimelineReset,
    kEnd,
  };

  struct WorkItem {
    WorkType type = WorkType::kEnd;
    std::optional<ffmpeg::Packet> packet;
    bool final_end = false;
  };

  void Run() noexcept;
  void RunLoop();
  bool SynchronizeBoundary(MwStreamerProcessorBoundaryReason reason);

  const std::size_t queue_capacity_;
  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  AudioProcessingChain processing_chain_;
  processor::StreamingProcessorHandler& processor_;
  common::Barrier& boundary_barrier_;
  OutputWorker& output_;
  performance::internal::TrackRecorder& performance_;
  std::function<void(const char* worker, const char* error)> on_failed_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_AUDIO_WORKER_H_

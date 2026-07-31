#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_WORKER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "mw/common/blocking_queue.h"
#include "mw/encoder/config.h"
#include "mw/ffmpeg/packet.h"
#include "mw/pipeline/internal/streaming/video_processing_chain.h"
#include "mw/processor/processor.h"

namespace mw::streamer::common {
class Barrier;
class Thread;
}  // namespace mw::streamer::common

namespace mw::streamer::decoder {
class VideoDecoder;
}

namespace mw::streamer::encoder {
class VideoEncoder;
}

namespace mw::streamer::processor {
class StreamingProcessorHandler;
}

namespace mw::streamer::pipeline::internal::streaming {

class OutputWorker;

class VideoWorker final {
 public:
  VideoWorker(
      std::unique_ptr<decoder::VideoDecoder> decoder,
      encoder::VideoEncoderConfig encoder_config, std::size_t queue_capacity,
      processor::StreamingProcessorHandler& processor,
      common::Barrier& boundary_barrier, OutputWorker& output,
      std::function<void(const char* worker, const char* error)> on_failed);
  ~VideoWorker();

  VideoWorker(const VideoWorker&) = delete;
  VideoWorker& operator=(const VideoWorker&) = delete;

  void Start();
  bool Input(const ffmpeg::Packet& packet);
  void Reset();
  void End(bool final_end);
  void RequestStop() noexcept;
  void Stop() noexcept;

 private:
  enum class WorkType {
    kPacket,
    kDecoderReset,
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
  void EncodeFrame(const ffmpeg::Frame& frame);

  const std::size_t queue_capacity_;
  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  std::unique_ptr<encoder::VideoEncoder> encoder_;
  VideoProcessingChain processing_chain_;
  processor::StreamingProcessorHandler& processor_;
  common::Barrier& boundary_barrier_;
  OutputWorker& output_;
  std::function<void(const char* worker, const char* error)> on_failed_;
  bool recovering_ = false;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_VIDEO_WORKER_H_

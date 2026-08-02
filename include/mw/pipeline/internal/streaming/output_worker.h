#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/common/blocking_queue.h"
#include "mw/encoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/pipeline/internal/streaming/frame_synchronizer.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::common {
class Thread;
}

namespace mw::streamer::encoder {
class AudioEncoder;
class VideoEncoder;
}  // namespace mw::streamer::encoder

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::output {
class OutputSession;
}

namespace mw::streamer::performance::internal {
class TrackRecorder;
}

namespace mw::streamer::pipeline::internal::streaming {

// Owns frame synchronization, both encoders, and OutputSession on one thread.
// AudioWorker and VideoWorker only deliver processed frames and boundaries.
class OutputWorker final {
 public:
  struct Callbacks {
    std::function<void()> on_ready;
    std::function<void()> on_completed;
    std::function<void(const char* error)> on_failed;
  };

  OutputWorker(int audio_stream_index, int video_stream_index,
               encoder::AudioEncoderConfig audio_encoder_config,
               encoder::VideoEncoderConfig video_encoder_config,
               std::vector<std::string> output_targets,
               zlm::OutputConfig zlm_config,
               std::size_t startup_packet_capacity,
               std::chrono::milliseconds max_track_wait, bool standby_enabled,
               std::string standby_image_path,
               const ffmpeg::HardwareContext* hardware_context,
               performance::internal::TrackRecorder* audio_performance,
               performance::internal::TrackRecorder* video_performance,
               std::shared_ptr<toolkit::EventPoller> poller,
               Callbacks callbacks);
  ~OutputWorker();

  OutputWorker(const OutputWorker&) = delete;
  OutputWorker& operator=(const OutputWorker&) = delete;

  void Start();
  void WriteAudio(const ffmpeg::Frame& frame);
  void WriteVideo(const ffmpeg::Frame& frame);
  void InterruptTrack(AVMediaType media_type);
  void FinishTrack(AVMediaType media_type);
  void RequestStop() noexcept;
  void Stop() noexcept;
  std::size_t queue_depth() const;
  std::shared_ptr<output::OutputSession> output_session() const;

 private:
  enum class WorkType {
    kFrame,
    kInterrupt,
    kFinish,
  };

  struct WorkItem {
    WorkType type;
    AVMediaType media_type;
    std::optional<ffmpeg::Frame> frame;
  };

  void Run() noexcept;
  void RunLoop();
  void HandleWork(WorkItem work);
  void DrainReadyFrames();
  void EncodeFrame(FrameSynchronizer::OutputFrame frame);
  void HandlePacket(const ffmpeg::Packet& packet);
  void OpenOutput();
  void CompleteOutput();
  void CloseOutput() noexcept;
  bool AllEncodersOpen() const noexcept;
  std::vector<ffmpeg::StreamInfo> EncodedStreams() const;

  const bool has_audio_;
  const bool has_video_;
  const std::vector<std::string> output_targets_;
  const zlm::OutputConfig zlm_config_;
  const std::size_t startup_packet_capacity_;
  const std::shared_ptr<toolkit::EventPoller> poller_;
  const Callbacks callbacks_;

  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  std::unique_ptr<encoder::AudioEncoder> audio_encoder_;
  std::unique_ptr<encoder::VideoEncoder> video_encoder_;
  FrameSynchronizer synchronizer_;
  performance::internal::TrackRecorder* audio_performance_;
  performance::internal::TrackRecorder* video_performance_;
  output::OutputSession* output_ = nullptr;
  std::shared_ptr<output::OutputSession> published_output_;
  std::vector<ffmpeg::Packet> pending_packets_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

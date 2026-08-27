#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "mw/common/blocking_queue.h"
#include "mw/encoder/config.h"
#include "mw/ffmpeg/frame.h"
#include "mw/pipeline/internal/streaming/frame_synchronizer.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::common {
class Thread;
}

namespace mw::streamer::ffmpeg {
class HardwareContext;
}

namespace mw::streamer::output {
class OutputSession;
class OutputSink;
namespace internal {
class OutputSinkWorker;
}
}  // namespace mw::streamer::output

namespace mw::streamer::performance::internal {
class TrackRecorder;
}

namespace mw::streamer::pipeline::internal::streaming {

class EncodedOutputSink;

// Owns frame synchronization and fans the resulting output timeline into
// independent raw and encoded sinks. AudioWorker and VideoWorker only deliver
// processed frames and boundaries.
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
               std::vector<std::unique_ptr<output::OutputSink>> output_sinks,
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
  void DispatchFrame(FrameSynchronizer::OutputFrame frame);
  void FinishSinks();
  void HandleEncodedReady();
  void HandleEncodedCompleted();
  void HandleEncodedFailed(const char* error) noexcept;
  void HandleRawCompleted(std::size_t index);
  void HandleRawFailed(std::size_t index, const char* error) noexcept;
  void MarkRawReady(std::size_t index);
  void MaybeNotifyReady();
  void MaybeNotifyCompleted();
  void AbortSinks() noexcept;

  const bool has_audio_;
  const bool has_video_;
  const Callbacks callbacks_;

  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  FrameSynchronizer synchronizer_;
  std::vector<std::unique_ptr<output::internal::OutputSinkWorker>> raw_sinks_;
  std::unique_ptr<EncodedOutputSink> encoded_sink_;

  mutable std::mutex sink_state_mutex_;
  std::vector<bool> raw_active_;
  std::vector<bool> raw_ready_;
  std::vector<bool> raw_completed_;
  bool encoded_active_ = false;
  bool encoded_ready_ = false;
  bool encoded_completed_ = false;
  bool ready_notified_ = false;
  bool completed_notified_ = false;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

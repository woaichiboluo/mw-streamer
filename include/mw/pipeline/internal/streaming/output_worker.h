#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

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
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/zlm/config.h"

namespace toolkit {
class EventPoller;
}

namespace mw::streamer::common {
class Thread;
}

namespace mw::streamer::output {
class OutputSession;
}

namespace mw::streamer::pipeline::internal::streaming {

// Owns OutputSession on one thread and serializes encoded packet delivery.
// Packets retain producer arrival order; this worker does not reorder by DTS.
class OutputWorker final {
 public:
  struct Callbacks {
    std::function<void()> on_ready;
    std::function<void()> on_completed;
    std::function<void(const char* error)> on_failed;
  };

  OutputWorker(bool has_audio, bool has_video,
               std::vector<std::string> output_targets,
               zlm::OutputConfig zlm_config,
               std::size_t startup_packet_capacity,
               std::shared_ptr<toolkit::EventPoller> poller,
               Callbacks callbacks);
  ~OutputWorker();

  OutputWorker(const OutputWorker&) = delete;
  OutputWorker& operator=(const OutputWorker&) = delete;

  void Start();
  void RegisterOutputStream(AVMediaType media_type,
                            const ffmpeg::StreamInfo& stream_info);
  void Write(const ffmpeg::Packet& packet);
  void EndTrack(AVMediaType media_type);
  void RequestStop() noexcept;
  void Stop() noexcept;

 private:
  enum class WorkType {
    kStreamReady,
    kPacket,
    kTrackEnd,
  };

  struct WorkItem {
    WorkType type;
    AVMediaType media_type;
    std::optional<ffmpeg::StreamInfo> stream_info;
    std::optional<ffmpeg::Packet> packet;
  };

  void Run() noexcept;
  void RunLoop();
  void HandleStreamReady(AVMediaType media_type,
                         ffmpeg::StreamInfo stream_info);
  void HandlePacket(ffmpeg::Packet packet);
  void HandleTrackEnd(AVMediaType media_type);
  void OpenOutput();
  void CloseOutput() noexcept;
  bool AllStreamsReady() const noexcept;
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
  std::optional<ffmpeg::StreamInfo> audio_stream_;
  std::optional<ffmpeg::StreamInfo> video_stream_;
  std::unique_ptr<output::OutputSession> output_;
  std::vector<ffmpeg::Packet> pending_packets_;
  bool audio_ended_ = false;
  bool video_ended_ = false;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_OUTPUT_WORKER_H_

#ifndef MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_ENCODED_OUTPUT_SINK_H_
#define MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_ENCODED_OUTPUT_SINK_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
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

namespace mw::streamer::output {
class OutputSession;
}

namespace mw::streamer::performance::internal {
class TrackRecorder;
}

namespace mw::streamer::pipeline::internal::streaming {

// Encodes synchronized frames on an isolated bounded queue and writes the
// resulting packets to one optional OutputSession.
class EncodedOutputSink final {
 public:
  struct Callbacks {
    std::function<void()> on_ready;
    std::function<void()> on_completed;
    std::function<void(const char* error)> on_failed;
  };

  EncodedOutputSink(int audio_stream_index, int video_stream_index,
                    encoder::AudioEncoderConfig audio_encoder_config,
                    encoder::VideoEncoderConfig video_encoder_config,
                    std::vector<std::string> output_targets,
                    zlm::OutputConfig zlm_config, std::size_t queue_capacity,
                    std::size_t startup_packet_capacity,
                    performance::internal::TrackRecorder* audio_performance,
                    performance::internal::TrackRecorder* video_performance,
                    std::shared_ptr<toolkit::EventPoller> poller,
                    Callbacks callbacks);
  ~EncodedOutputSink();

  EncodedOutputSink(const EncodedOutputSink&) = delete;
  EncodedOutputSink& operator=(const EncodedOutputSink&) = delete;

  void Start();
  bool Write(const ffmpeg::Frame& frame, AVMediaType media_type,
             bool force_key_frame = false);
  // Preserves queued frames, then drains the encoders and completes output.
  void Finish();
  // Discards queued frames and stops without completing output.
  void Abort() noexcept;
  void Stop() noexcept;
  std::size_t queue_depth() const;
  std::shared_ptr<output::OutputSession> output_session() const;

 private:
  struct WorkItem {
    ffmpeg::Frame frame;
    AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
    bool force_key_frame = false;
  };

  void Run() noexcept;
  void RunLoop();
  void EncodeFrame(WorkItem work);
  void HandlePacket(const ffmpeg::Packet& packet);
  void PrepareOutput();
  void CompleteOutput();
  void CloseOutput() noexcept;
  void ReportFailure(const char* error) noexcept;
  bool AllEncodersOpen() const noexcept;
  std::vector<ffmpeg::StreamInfo> EncodedStreams() const;

  const std::size_t queue_capacity_;
  const std::vector<std::string> output_targets_;
  const zlm::OutputConfig zlm_config_;
  const std::size_t startup_packet_capacity_;
  const std::shared_ptr<toolkit::EventPoller> poller_;
  const Callbacks callbacks_;

  common::BlockingQueue<WorkItem> queue_;
  std::unique_ptr<common::Thread> thread_;
  std::unique_ptr<encoder::AudioEncoder> audio_encoder_;
  std::unique_ptr<encoder::VideoEncoder> video_encoder_;
  performance::internal::TrackRecorder* audio_performance_;
  performance::internal::TrackRecorder* video_performance_;
  std::atomic<bool> aborted_ = false;
  std::atomic<bool> failure_reported_ = false;
  bool ready_ = false;
  output::OutputSession* output_ = nullptr;
  std::shared_ptr<output::OutputSession> published_output_;
  std::vector<ffmpeg::Packet> pending_packets_;
};

}  // namespace mw::streamer::pipeline::internal::streaming

#endif  // MW_STREAMER_INCLUDE_MW_PIPELINE_INTERNAL_STREAMING_ENCODED_OUTPUT_SINK_H_

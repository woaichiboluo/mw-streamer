#ifndef MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_
#define MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/encoder.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/pipeline/pipeline_state.h"
#include "mw/streamer/processor/processor.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace mw::streamer {

struct MW_STREAMER_API LivePipelineConfig {
  FfmpegInputConfig input;  // Only kLive is accepted.
  SchedulerConfig scheduler;
  // fps must equal scheduler.video_frame_rate. Video size/format describe the
  // processed frames, with Encoder's existing inheritance defaults. Audio is
  // S32 at the channel's fixed 48 kHz; Encoder converts to its native format.
  EncoderConfig encoder;
};

struct LivePipelineReadyInfo {
  // Source, negotiated Scheduler and actual Encoder descriptions.
  // All snapshots remain queryable after stopping.
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::StreamInfo> scheduled_streams;
  std::vector<ffmpeg::StreamInfo> encoded_streams;
  std::vector<std::string> output_urls;
  std::string rtsp_url;
  std::string hls_url;
};

// Input(kLive) -> Scheduler -> Processor -> Encoder -> AsyncRemuxer.
// Requires Init before Start and destruction before Shutdown. Configure
// callbacks before Start. Processor callbacks obey its noexcept contract and
// consume their own exceptions. Audio/video processing runs serially on one
// channel worker, and the resulting picture/audio are submitted as one
// MediaFrame to the realtime Encoder consumer; OnReady runs after native filter
// negotiation, End/Stop on the cleanup worker. OnReady receives actual
// scheduled frame descriptions. Callbacks may query snapshots, but must not
// call lifecycle/configuration methods or destroy the pipeline. State observer
// exceptions are logged.
class MW_STREAMER_API LivePipeline final {
 public:
  using OnSelectStreams = FfmpegInput::OnSelectStreams;
  using OnReady = Processor::OnReady;
  using OnVideo = Processor::OnVideo;
  using OnAudio = Processor::OnAudio;
  using OnEnded = Processor::OnEnded;
  using OnStop = Processor::OnStop;
  using OnStateChanged = std::function<void(const PipelineStatus&)>;

  explicit LivePipeline(LivePipelineConfig config = {});
  // Retains the shared video device for Input and Encoder. NVENC selection and
  // its surface pixel layout remain explicit Encoder configuration.
  explicit LivePipeline(const ffmpeg::HwDeviceContext& video_device,
                        LivePipelineConfig config = {});
  ~LivePipeline();
  LivePipeline(const LivePipeline&) = delete;
  LivePipeline& operator=(const LivePipeline&) = delete;
  LivePipeline(LivePipeline&&) = delete;
  LivePipeline& operator=(LivePipeline&&) = delete;

  // Before Start, choose source indices from all audio/video descriptions.
  // An empty callback restores Input's first-video/first-audio default.
  void SetOnSelectStreams(OnSelectStreams callback);
  void SetOnReady(OnReady callback);
  void SetOnVideo(OnVideo callback);
  void SetOnAudio(OnAudio callback);
  void SetOnEnded(OnEnded callback);
  void SetOnStop(OnStop callback);
  void SetOnStateChanged(OnStateChanged callback);

  // Output registration, runtime addition, TLS and Stop/reset semantics match
  // RemuxPipeline. Returned paths/addresses and ready_info survive Stop.
  std::string AddPushUrl(const std::string& url);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port,
                      bool tls = false);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls = false);

  // Waits for every stage and the first locally accepted encoded packet, or
  // successful prepared EOF. Remote push connection success is not required.
  // Input's timeout/retry policy applies; another thread may Stop this wait.
  // Any stage's false Start result is failure. Cleanup precedes throwing.
  void Start(std::string_view url);
  // Stops Input and Scheduler, waits for current filters, then drains accepted
  // Encoder frames/codec delay and Remuxer packets. Only normal EOF calls
  // Processor.End first. Fatal errors cancel Encoder work. Terminal states
  // follow cleanup; no callbacks run after return. Serialize Start calls.
  // Restart retains callbacks but requires adding outputs again.
  void Stop();

  PipelineState state() const noexcept;
  PipelineStatus status() const;
  LivePipelineReadyInfo ready_info() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_LIVE_PIPELINE_H_

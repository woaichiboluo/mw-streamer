#ifndef MW_STREAMER_PIPELINE_SINGLE_LIVE_PIPELINE_H_
#define MW_STREAMER_PIPELINE_SINGLE_LIVE_PIPELINE_H_

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mw/export.h"
#include "mw/streamer/encoder/av_encoder_config.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/input/av_media_input.h"
#include "mw/streamer/output/output.h"
#include "mw/streamer/remuxer/remuxer.h"

namespace mw::streamer {

struct SingleLivePipelineConfig {
  AvInputProxyConfig input;
  AvDecoderConfig video_decoder;
  AvDecoderConfig audio_decoder;
  // One format for input conversion, scheduling and encoding. Enabled video
  // requires explicit dimensions. Input does not resize/convert video pixels;
  // the video callback must produce this format if the source differs.
  // The constructor deep-copies the borrowed channel layout.
  AvEncoderConfig output;
};

enum class SingleLivePipelineState { kStopped, kRunning, kFailed };

// Owns one Live Input, Scheduler, Output and EncodePushSink. No extra media
// queues or workers. The shared CPU/CUDA device is retained by reference.
// Requires Init before Start; destroy the pipeline before Shutdown.
// Serialize lifecycle, configuration and output additions externally. Queries
// and RequestStop may run concurrently, including from callbacks.
class MW_STREAMER_API SingleLivePipeline final {
 public:
  using OnInitialize = std::function<void()>;
  using OnStop = std::function<void()>;
  using OnEnded = std::function<void()>;
  using OnError = std::function<void(std::exception_ptr)>;
  using OnVideo = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;
  using OnAudio = std::function<ffmpeg::Frame(const ffmpeg::Frame&)>;

  explicit SingleLivePipeline(const ffmpeg::HwDeviceContext& device,
                              SingleLivePipelineConfig config = {});
  ~SingleLivePipeline();
  SingleLivePipeline(const SingleLivePipeline&) = delete;
  SingleLivePipeline& operator=(const SingleLivePipeline&) = delete;
  SingleLivePipeline(SingleLivePipeline&&) = delete;
  SingleLivePipeline& operator=(SingleLivePipeline&&) = delete;

  // Configure before Start or after Stop. No callback may reenter lifecycle,
  // configuration, output additions or destruction on this pipeline.
  // Initialize runs synchronously before any media component starts. Failure
  // may throw; it must clean its own partial resources with RAII. After it
  // succeeds (including an absent callback), Stop is called exactly once on
  // the Start/Stop caller, after media teardown, including startup rollback.
  // Stop/Ended/Error observer exceptions are logged and do not escape.
  void SetOnInitialize(OnInitialize callback);
  void SetOnStop(OnStop callback);
  // Input's final kEnded only, once per session on its controller thread.
  // Loop boundaries and explicit cancellation do not notify. The channel
  // keeps running with held video/silent audio until explicitly stopped.
  void SetOnEnded(OnEnded callback);
  // Once per first terminal Input/Sink error, on the reporting worker outside
  // locks. State/error are published and cancellation requested before notice.
  // Synchronous Start failures return false without this notification.
  void SetOnError(OnError callback);
  // Read-only source frames; callbacks must not throw. Audio/video may run
  // concurrently. Called only when the corresponding input track has data;
  // absent tracks use Scheduler's held/default picture or silence. Output
  // audio must retain the prepared sample count. Empty callbacks select the
  // original track. Source PTS is available on the frame, not an encode clock.
  void SetOnVideo(OnVideo callback);
  void SetOnAudio(OnAudio callback);

  // Returns after local output initialization and worker startup, not input
  // connection or remote push success. Active sessions reject repeated Start
  // without changing state. Stop is required after asynchronous failure or
  // RequestStop; a synchronous startup failure rolls back and permits retry.
  // Restart rebuilds components, retains callbacks/targets, clears errors and
  // starts new recording files. Input URL is fixed within each session.
  // Asynchronous errors may notify before Start returns.
  [[nodiscard]] bool Start(std::string_view url) noexcept;
  void RequestStop() noexcept;
  // Joins scheduling/input, drains encoding/muxing, then calls business Stop.
  // Repeated calls are harmless. Failed state/error survive cleanup. Returned
  // state is kStopped for a normal stop; Sink returns kWaiting after cleanup
  // unless it failed. No business callbacks remain after returning.
  void Stop() noexcept;

  SingleLivePipelineState state() const;
  AvMediaInputStatus input_status() const;
  FrameSinkStatus sink_status() const;
  std::exception_ptr error() const;

  // Same synchronous exceptions and return values as EncodePushSink. All
  // successfully added targets, including dynamic additions, survive restart;
  // failed additions are not retained. Publication is independent of remote
  // pusher status. Additions after RequestStop/terminal failure require Stop.
  std::string AddPushUrl(const std::string& url);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port);
  void AddRtspPublish(const std::string& app, const std::string& stream,
                      const std::string& ip, std::uint16_t port, bool tls);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port);
  std::string AddHlsPublish(const std::string& app, const std::string& stream,
                            const std::string& ip, std::uint16_t port,
                            bool tls);
  // Retains the last planned/actual addresses after Stop.
  Remuxer::OutputInfo output_info() const;
  // Same raw SDK semantics as Remuxer; empty while tearing down or stopped.
  std::vector<Remuxer::PushStatus> push_statuses() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_SINGLE_LIVE_PIPELINE_H_

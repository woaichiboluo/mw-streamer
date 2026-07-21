#ifndef MW_STREAMER_PIPELINE_CONFIG_H_
#define MW_STREAMER_PIPELINE_CONFIG_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mw/streamer/processor.h"
#include "mw/streamer/status.h"

namespace mw::streamer {

enum class InputProtocol {
  kRtmp,
  kRtsp,
  kSrt,
  kFile,
  kHls,
};

enum class HlsMode {
  kLive,
  kVod,
};

enum class OutputProtocol {
  kRtmp,
  kRtsp,
  kSrt,
  kFile,
};

enum class RtspTransport {
  kTcp,
  kUdp,
};

enum class VideoCodec {
  kH264,
  kH265,
};

enum class VideoRateControl {
  kCbr,
  kVbr,
};

struct ReconnectConfig {
  std::size_t max_retries = 0;
  std::chrono::milliseconds initial_backoff{1'000};
  std::chrono::milliseconds maximum_backoff{30'000};
  double jitter_ratio = 0.10;
};

struct InputConfig {
  std::string id;
  InputProtocol protocol = InputProtocol::kFile;
  HlsMode hls_mode = HlsMode::kLive;
  std::string url;
  RtspTransport rtsp_transport = RtspTransport::kTcp;
  ReconnectConfig reconnect;
  std::chrono::milliseconds open_timeout{10'000};
  std::chrono::milliseconds read_timeout{5'000};
  std::size_t packet_queue_capacity = 512;
  std::map<std::string, std::string> options;
};

struct OutputConfig {
  std::string id;
  OutputProtocol protocol = OutputProtocol::kFile;
  std::string url;
  RtspTransport rtsp_transport = RtspTransport::kTcp;
  ReconnectConfig reconnect;
  std::chrono::milliseconds open_timeout{10'000};
  std::chrono::milliseconds write_timeout{5'000};
  std::size_t packet_queue_capacity = 512;
  std::map<std::string, std::string> options;
};

struct VideoEncoderConfig {
  VideoCodec codec = VideoCodec::kH264;
  int width = 0;
  int height = 0;
  MwRational frame_rate{0, 1};
  std::int64_t bit_rate = 4'000'000;
  std::int64_t maximum_bit_rate = 0;
  std::int64_t vbv_buffer_size = 0;
  // Zero selects a two-second GOP after frame_rate is known.
  int gop_size = 0;
  int maximum_b_frames = 0;
  VideoRateControl rate_control = VideoRateControl::kCbr;
  std::string preset = "p4";
  std::map<std::string, std::string> options;
};

struct AudioEncoderConfig {
  std::int64_t bit_rate = 128'000;
  int sample_rate = 48'000;
  int channel_count = 2;
};

struct SynchronizationConfig {
  std::size_t primary_video_input_index = 0;
  std::chrono::milliseconds pts_tolerance{5};
  std::chrono::milliseconds maximum_wait{100};
};

struct QueueConfig {
  std::size_t decoded_video_capacity = 8;
  std::size_t audio_fifo_capacity_frames = 48'000;
};

enum class PipelineState {
  kCreated,
  kStarting,
  kRunning,
  kStopping,
  kCompleted,
  kFailed,
  kStopped,
};

enum class EndpointKind {
  kInput,
  kOutput,
};

enum class EndpointEventType {
  kReconnecting,
  kRecovered,
  kFailed,
};

struct EndpointEvent {
  EndpointKind kind = EndpointKind::kInput;
  EndpointEventType type = EndpointEventType::kFailed;
  std::string endpoint_id;
  Status status;
};

struct PipelineCallbacks {
  std::function<void(PipelineState state, const Status& status)>
      on_state_changed;
  std::function<void(const EndpointEvent& event)> on_endpoint_event;
};

struct PipelineConfig {
  std::string id;
  int device_id = 0;
  std::vector<InputConfig> inputs;
  std::vector<OutputConfig> outputs;
  VideoEncoderConfig video_encoder;
  AudioEncoderConfig audio_encoder;
  SynchronizationConfig synchronization;
  QueueConfig queues;
  std::chrono::milliseconds live_delay{0};
  std::chrono::milliseconds standby_timeout{2'000};
  std::string standby_image_path;
  MwProcessorCallbacks processor_callbacks{};
  std::string processor_initial_config;
  PipelineCallbacks callbacks;
};

}  // namespace mw::streamer

#endif  // MW_STREAMER_PIPELINE_CONFIG_H_

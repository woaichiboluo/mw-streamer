#include "mw/streamer/internal/pipeline_config_validator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_set>

#include "fmt/format.h"
#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/input_mode.h"

namespace mw::streamer::internal {
namespace {

void ValidateReconnectConfig(const ReconnectConfig& config,
                             const std::string& endpoint_id) {
  if (config.initial_backoff.count() <= 0 ||
      config.maximum_backoff < config.initial_backoff ||
      !std::isfinite(config.jitter_ratio) || config.jitter_ratio < 0.0 ||
      config.jitter_ratio > 1.0) {
    ThrowError(StatusCode::kInvalidArgument,
               fmt::format("endpoint '{}' has invalid reconnect settings",
                           endpoint_id));
  }
}

void ValidateInput(const InputConfig& input) {
  if (input.id.empty() || input.url.empty() ||
      input.packet_queue_capacity == 0 || input.open_timeout.count() < 0 ||
      input.read_timeout.count() < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "input id, URL, and packet queue capacity are required");
  }
  switch (input.protocol) {
    case InputProtocol::kRtmp:
    case InputProtocol::kSrt:
    case InputProtocol::kFile:
    case InputProtocol::kHls:
      if (input.rtsp_transport != RtspTransport::kTcp) {
        ThrowError(StatusCode::kInvalidArgument,
                   fmt::format("non-RTSP input '{}' cannot select RTSP UDP",
                               input.id));
      }
      break;
    case InputProtocol::kRtsp:
      if (input.rtsp_transport != RtspTransport::kTcp &&
          input.rtsp_transport != RtspTransport::kUdp) {
        ThrowError(
            StatusCode::kInvalidArgument,
            fmt::format("RTSP input '{}' has an unknown transport", input.id));
      }
      break;
    default:
      ThrowError(StatusCode::kInvalidArgument,
                 fmt::format("input '{}' has an unknown protocol", input.id));
  }
  if (input.hls_mode != HlsMode::kLive && input.hls_mode != HlsMode::kVod) {
    ThrowError(StatusCode::kInvalidArgument,
               fmt::format("input '{}' has an unknown HLS mode", input.id));
  }
  if (input.protocol != InputProtocol::kHls &&
      input.hls_mode != HlsMode::kLive) {
    ThrowError(
        StatusCode::kInvalidArgument,
        fmt::format("non-HLS input '{}' cannot select HLS VOD mode", input.id));
  }
  ValidateReconnectConfig(input.reconnect, input.id);
}

void ValidateOutput(const OutputConfig& output) {
  if (output.id.empty() || output.url.empty() ||
      output.packet_queue_capacity == 0 || output.open_timeout.count() < 0 ||
      output.write_timeout.count() < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "output id, URL, packet queue capacity, and nonnegative "
               "timeouts are required");
  }
  switch (output.protocol) {
    case OutputProtocol::kRtmp:
    case OutputProtocol::kSrt:
    case OutputProtocol::kFile:
      if (output.rtsp_transport != RtspTransport::kTcp) {
        ThrowError(StatusCode::kInvalidArgument,
                   fmt::format("non-RTSP output '{}' cannot select RTSP UDP",
                               output.id));
      }
      break;
    case OutputProtocol::kRtsp:
      if (output.rtsp_transport != RtspTransport::kTcp &&
          output.rtsp_transport != RtspTransport::kUdp) {
        ThrowError(StatusCode::kInvalidArgument,
                   fmt::format("RTSP output '{}' has an unknown transport",
                               output.id));
      }
      break;
    default:
      ThrowError(StatusCode::kInvalidArgument,
                 fmt::format("output '{}' has an unknown protocol", output.id));
  }
  ValidateReconnectConfig(output.reconnect, output.id);
}

void ValidateVideoEncoder(const VideoEncoderConfig& config) {
  if (config.codec != VideoCodec::kH264 && config.codec != VideoCodec::kH265) {
    ThrowError(StatusCode::kInvalidArgument, "video encoder codec is unknown");
  }
  if (config.rate_control != VideoRateControl::kCbr &&
      config.rate_control != VideoRateControl::kVbr) {
    ThrowError(StatusCode::kInvalidArgument,
               "video encoder rate control mode is unknown");
  }
  const bool valid_preset = config.preset == "p1" || config.preset == "p2" ||
                            config.preset == "p3" || config.preset == "p4" ||
                            config.preset == "p5" || config.preset == "p6" ||
                            config.preset == "p7";
  if (config.width <= 0 || config.height <= 0 ||
      config.frame_rate.numerator <= 0 || config.frame_rate.denominator <= 0 ||
      config.bit_rate <= 0 || config.maximum_bit_rate < 0 ||
      config.vbv_buffer_size < 0 || config.gop_size < 0 ||
      config.maximum_b_frames < 0 || !valid_preset) {
    ThrowError(StatusCode::kInvalidArgument,
               "video encoder configuration is invalid");
  }
}

}  // namespace

void ValidatePipelineConfig(const PipelineConfig& config) {
  if (config.id.empty()) {
    ThrowError(StatusCode::kInvalidArgument, "pipeline id is required");
  }
  if (config.device_id < 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "pipeline NVIDIA device id must not be negative");
  }
  if (config.inputs.empty() || config.outputs.empty()) {
    ThrowError(StatusCode::kInvalidArgument,
               "pipeline requires at least one input and one output");
  }
  if (config.synchronization.primary_video_input_index >=
      config.inputs.size()) {
    ThrowError(StatusCode::kInvalidArgument,
               "primary video input index is out of range");
  }
  if (config.synchronization.pts_tolerance.count() < 0 ||
      config.synchronization.maximum_wait.count() <= 0 ||
      config.live_delay.count() < 0 || config.standby_timeout.count() <= 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "pipeline timing configuration is invalid");
  }
  if (config.queues.decoded_video_capacity == 0 ||
      config.queues.audio_fifo_capacity_frames <
          kProcessorAudioBlockFrameCount) {
    ThrowError(StatusCode::kInvalidArgument,
               "pipeline queue configuration is invalid");
  }
  if (config.standby_image_path.empty()) {
    ThrowError(StatusCode::kInvalidArgument,
               "pipeline standby image path is required");
  }

  ValidateVideoEncoder(config.video_encoder);
  if (config.audio_encoder.bit_rate <= 0 ||
      config.audio_encoder.sample_rate != kProcessorAudioSampleRate ||
      config.audio_encoder.channel_count != kProcessorAudioChannelCount) {
    ThrowError(StatusCode::kInvalidArgument,
               "audio encoder must use AAC-LC at 48 kHz stereo");
  }

  std::unordered_set<std::string> endpoint_ids;
  for (const InputConfig& input : config.inputs) {
    ValidateInput(input);
    if (!endpoint_ids.insert(input.id).second) {
      ThrowError(StatusCode::kInvalidArgument,
                 fmt::format("duplicate endpoint id '{}'", input.id));
    }
  }
  if (config.live_delay.count() > 0 &&
      !std::all_of(config.inputs.begin(), config.inputs.end(), IsLiveInput)) {
    ThrowError(StatusCode::kInvalidArgument,
               "live delay requires every input to use live mode");
  }
  for (const OutputConfig& output : config.outputs) {
    ValidateOutput(output);
    if (!endpoint_ids.insert(output.id).second) {
      ThrowError(StatusCode::kInvalidArgument,
                 fmt::format("duplicate endpoint id '{}'", output.id));
    }
  }
}

}  // namespace mw::streamer::internal

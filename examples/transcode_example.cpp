#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "fmt/format.h"
#include "mw/streamer/pipeline.h"
#include "mw/streamer/pipeline_config.h"
#include "mw/streamer/status.h"

namespace {

bool ParsePositiveInt(std::string_view text, int* value) {
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, *value);
  return error == std::errc{} && position == end && *value > 0;
}

bool ParseNonNegativeInt(std::string_view text, int* value) {
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, *value);
  return error == std::errc{} && position == end && *value >= 0;
}

bool ParseProtocol(std::string_view text, mw::streamer::InputProtocol* protocol,
                   mw::streamer::RtspTransport* rtsp_transport,
                   mw::streamer::HlsMode* hls_mode) {
  if (text == "file") {
    *protocol = mw::streamer::InputProtocol::kFile;
  } else if (text == "rtsp" || text == "rtsp-tcp") {
    *protocol = mw::streamer::InputProtocol::kRtsp;
    *rtsp_transport = mw::streamer::RtspTransport::kTcp;
  } else if (text == "rtsp-udp") {
    *protocol = mw::streamer::InputProtocol::kRtsp;
    *rtsp_transport = mw::streamer::RtspTransport::kUdp;
  } else if (text == "rtmp") {
    *protocol = mw::streamer::InputProtocol::kRtmp;
  } else if (text == "srt") {
    *protocol = mw::streamer::InputProtocol::kSrt;
  } else if (text == "hls" || text == "hls-live") {
    *protocol = mw::streamer::InputProtocol::kHls;
    *hls_mode = mw::streamer::HlsMode::kLive;
  } else if (text == "hls-vod") {
    *protocol = mw::streamer::InputProtocol::kHls;
    *hls_mode = mw::streamer::HlsMode::kVod;
  } else {
    return false;
  }
  return true;
}

bool ParseOutputProtocol(std::string_view text,
                         mw::streamer::OutputProtocol* protocol,
                         mw::streamer::RtspTransport* rtsp_transport) {
  if (text == "file") {
    *protocol = mw::streamer::OutputProtocol::kFile;
  } else if (text == "rtsp" || text == "rtsp-tcp") {
    *protocol = mw::streamer::OutputProtocol::kRtsp;
    *rtsp_transport = mw::streamer::RtspTransport::kTcp;
  } else if (text == "rtsp-udp") {
    *protocol = mw::streamer::OutputProtocol::kRtsp;
    *rtsp_transport = mw::streamer::RtspTransport::kUdp;
  } else if (text == "rtmp") {
    *protocol = mw::streamer::OutputProtocol::kRtmp;
  } else if (text == "srt") {
    *protocol = mw::streamer::OutputProtocol::kSrt;
  } else {
    return false;
  }
  return true;
}

void PrintUsage(std::string_view program) {
  fmt::print(stderr,
             "usage: {} <file|rtsp[-tcp|-udp]|rtmp|srt|hls[-live|-vod]> "
             "<input-url> "
             "<output-url> "
             "<standby.jpg|png> <width> <height> <fps> "
             "<run-seconds,0=until-eof> [h264|h265] [live-delay-ms] "
             "[max-retries,0=unlimited] "
             "[output-protocol:file|rtsp[-tcp|-udp]|rtmp|srt] "
             "[additional-output-protocol additional-output-url]...\n",
             program);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 9 || (argc > 13 && (argc - 13) % 2 != 0)) {
    PrintUsage(argv[0]);
    return 2;
  }

  mw::streamer::InputProtocol protocol;
  int width = 0;
  int height = 0;
  int frame_rate = 0;
  int run_seconds = 0;
  int live_delay_ms = 0;
  int maximum_retries = 0;
  mw::streamer::RtspTransport input_rtsp_transport =
      mw::streamer::RtspTransport::kTcp;
  mw::streamer::HlsMode hls_mode = mw::streamer::HlsMode::kLive;
  mw::streamer::OutputProtocol output_protocol =
      mw::streamer::OutputProtocol::kFile;
  mw::streamer::RtspTransport output_rtsp_transport =
      mw::streamer::RtspTransport::kTcp;
  if (!ParseProtocol(argv[1], &protocol, &input_rtsp_transport, &hls_mode) ||
      !ParsePositiveInt(argv[5], &width) ||
      !ParsePositiveInt(argv[6], &height) ||
      !ParsePositiveInt(argv[7], &frame_rate) ||
      !ParseNonNegativeInt(argv[8], &run_seconds) ||
      (argc >= 11 && !ParseNonNegativeInt(argv[10], &live_delay_ms)) ||
      (argc >= 12 && !ParseNonNegativeInt(argv[11], &maximum_retries)) ||
      (argc >= 13 && !ParseOutputProtocol(argv[12], &output_protocol,
                                          &output_rtsp_transport))) {
    PrintUsage(argv[0]);
    return 2;
  }

  mw::streamer::VideoCodec codec = mw::streamer::VideoCodec::kH264;
  if (argc >= 10) {
    const std::string_view codec_name(argv[9]);
    if (codec_name == "h265") {
      codec = mw::streamer::VideoCodec::kH265;
    } else if (codec_name != "h264") {
      PrintUsage(argv[0]);
      return 2;
    }
  }

  mw::streamer::PipelineConfig config;
  config.id = "transcode-example";
  mw::streamer::InputConfig input;
  input.id = "input";
  input.protocol = protocol;
  input.hls_mode = hls_mode;
  input.rtsp_transport = input_rtsp_transport;
  input.url = argv[2];
  input.reconnect.max_retries = static_cast<std::size_t>(maximum_retries);
  config.inputs.push_back(std::move(input));
  mw::streamer::OutputConfig output;
  output.id = "recording";
  output.protocol = output_protocol;
  output.rtsp_transport = output_rtsp_transport;
  output.url = argv[3];
  config.outputs.push_back(std::move(output));
  for (int argument_index = 13; argument_index < argc; argument_index += 2) {
    mw::streamer::OutputProtocol additional_protocol;
    mw::streamer::RtspTransport additional_rtsp_transport =
        mw::streamer::RtspTransport::kTcp;
    if (!ParseOutputProtocol(argv[argument_index], &additional_protocol,
                             &additional_rtsp_transport)) {
      PrintUsage(argv[0]);
      return 2;
    }
    mw::streamer::OutputConfig additional_output;
    additional_output.id = fmt::format("output-{}", config.outputs.size());
    additional_output.protocol = additional_protocol;
    additional_output.rtsp_transport = additional_rtsp_transport;
    additional_output.url = argv[argument_index + 1];
    config.outputs.push_back(std::move(additional_output));
  }
  config.video_encoder.codec = codec;
  config.video_encoder.width = width;
  config.video_encoder.height = height;
  config.video_encoder.frame_rate = {frame_rate, 1};
  config.standby_image_path = argv[4];
  config.live_delay = std::chrono::milliseconds(live_delay_ms);
  config.callbacks.on_state_changed = [](mw::streamer::PipelineState state,
                                         const mw::streamer::Status& status) {
    fmt::print("pipeline state={} status={} message={}\n",
               static_cast<int>(state), static_cast<int>(status.code()),
               status.message());
  };
  config.callbacks.on_endpoint_event =
      [](const mw::streamer::EndpointEvent& event) {
        fmt::print("endpoint={} event={} status={} message={}\n",
                   event.endpoint_id, static_cast<int>(event.type),
                   static_cast<int>(event.status.code()),
                   event.status.message());
      };

  mw::streamer::Pipeline pipeline(std::move(config));
  const mw::streamer::Status start_status = pipeline.Start();
  if (!start_status.ok()) {
    fmt::print(stderr, "pipeline start failed: {}\n", start_status.message());
    return 1;
  }

  mw::streamer::Status terminal_status;
  if (run_seconds == 0) {
    terminal_status = pipeline.Wait();
  } else {
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
    terminal_status = pipeline.Stop();
  }
  if (!terminal_status.ok()) {
    fmt::print(stderr, "pipeline failed: {}\n", terminal_status.message());
    return 1;
  }
  fmt::print("pipeline finished successfully\n");
  return 0;
}

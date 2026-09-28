#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "mw/streamer/api.h"
#include "mw/streamer/input/zlm_input.h"
#include "mw/streamer/output/rtsp_publish_sink.h"
#include "mw/streamer/pipeline/pipeline.h"

namespace {

using mw::streamer::InputState;
using mw::streamer::PacketSinkState;
using mw::streamer::Pipeline;
using mw::streamer::PipelineState;
using mw::streamer::RtspPublishSink;
using mw::streamer::RtspPublishSinkConfig;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;

int Run(const char* input_url, const char* app, const char* stream,
        const char* port_text) {
  RtspPublishSinkConfig config;
  config.app = app;
  config.stream = stream;
  if (port_text != nullptr) {
    const auto port = std::stoi(port_text);
    if (port < 1 || port > 65535) {
      throw std::invalid_argument("RTSP端口必须在1到65535之间");
    }
    config.port = static_cast<std::uint16_t>(port);
  }
  const auto port = config.port;
  ZlmInputConfig input;
  input.url = input_url;
  Pipeline pipeline(std::make_unique<ZlmInput>(std::move(input)));
  auto sink = std::make_unique<RtspPublishSink>("rtsp", std::move(config));
  auto* output = sink.get();
  pipeline.AddSink(std::move(sink));
  pipeline.Start();
  fmt::print("RTSP发布地址：rtsp://127.0.0.1:{}/{}/{}\n", port, app, stream);
  while (pipeline.state() != PipelineState::kFailed &&
         pipeline.input_status().state != InputState::kFailed &&
         output->state() != PacketSinkState::kEnded &&
         output->state() != PacketSinkState::kFailed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const auto input_status = pipeline.input_status();
  pipeline.Stop();
  if (pipeline.state() == PipelineState::kFailed) {
    fmt::print(stderr, "链路失败：{}\n", pipeline.error());
    return 1;
  }
  if (input_status.state == InputState::kFailed) {
    fmt::print(stderr, "输入失败：{}\n", input_status.error);
    return 1;
  }
  if (output->state() == PacketSinkState::kFailed) {
    fmt::print(stderr, "RTSP发布失败：{}\n", output->error());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4 && argc != 5) {
    fmt::print(stderr, "用法：{} input_url app stream [port]\n", argv[0]);
    return 2;
  }
  MwLogConfig log_config;
  mw_log_default_config(&log_config);
  MwZlmConfig zlm_config;
  mw_zlm_default_config(&zlm_config);
  if (!mw_streamer_initialize(&log_config, &zlm_config)) {
    fmt::print(stderr, "运行时初始化失败：{}\n", mw_last_error());
    return 1;
  }
  int result = 1;
  try {
    result = Run(argv[1], argv[2], argv[3], argc == 5 ? argv[4] : nullptr);
  } catch (const std::exception& error) {
    fmt::print(stderr, "运行失败：{}\n", error.what());
  }
  mw_streamer_shutdown();
  if (mw_streamer_is_initialized()) result = 1;
  return result;
}

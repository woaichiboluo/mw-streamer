#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "mw/init/init.h"
#include "mw/input/zlm_input.h"
#include "mw/output/remux_sink.h"
#include "mw/pipeline/pipeline.h"

namespace {

using mw::streamer::input::InputState;
using mw::streamer::input::ZlmInput;
using mw::streamer::input::ZlmInputConfig;
using mw::streamer::output::RemuxSink;
using mw::streamer::output::RemuxSinkConfig;
using mw::streamer::sink::PacketSinkState;
using namespace mw::streamer::pipeline;

volatile std::sig_atomic_t interrupted = 0;

void OnSignal(int) { interrupted = 1; }

bool IsFinished(const RemuxSink& sink) {
  return sink.state() == PacketSinkState::kEnded ||
         sink.state() == PacketSinkState::kFailed;
}

int Run(int argc, char* argv[]) {
  ZlmInputConfig input;
  input.url = argv[1];
  Pipeline pipeline(std::make_unique<ZlmInput>(std::move(input)));
  // Pipeline owns the sinks; these borrowed pointers are only status views.
  std::vector<RemuxSink*> outputs;
  for (int i = 2; i < argc; ++i) {
    RemuxSinkConfig config;
    config.target = argv[i];
    auto sink = std::make_unique<RemuxSink>(fmt::format("remux-{}", i - 2),
                                            std::move(config));
    outputs.push_back(sink.get());
    pipeline.AddSink(std::move(sink));
  }
  pipeline.Start();
  while (!interrupted && pipeline.state() != PipelineState::kFailed &&
         pipeline.input_status().state != InputState::kFailed &&
         !std::all_of(outputs.begin(), outputs.end(),
                      [](const auto* sink) { return IsFinished(*sink); })) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const auto input_status = pipeline.input_status();
  pipeline.Stop();
  bool failed = false;
  if (pipeline.state() == PipelineState::kFailed) {
    fmt::print(stderr, "链路失败：{}\n", pipeline.error());
    failed = true;
  }
  if (input_status.state == InputState::kFailed) {
    fmt::print(stderr, "输入失败：{}\n", input_status.error);
    failed = true;
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (outputs[i]->state() == PacketSinkState::kFailed) {
      fmt::print(stderr, "输出失败：{}，{}\n", argv[i + 2],
                 outputs[i]->error());
      failed = true;
    } else {
      fmt::print("输出已关闭：{}\n", argv[i + 2]);
    }
  }
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fmt::print(stderr, "用法：{} input_url target [target...]\n", argv[0]);
    return 2;
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  int result = 1;
  try {
    mw::streamer::Init();
    result = Run(argc, argv);
  } catch (const std::exception& error) {
    fmt::print(stderr, "运行失败：{}\n", error.what());
  }
  mw::streamer::Shutdown();
  return result;
}

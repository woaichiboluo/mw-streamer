#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "mw/streamer/input/zlm_input.h"
#include "mw/streamer/output/remux_sink.h"
#include "mw/streamer/pipeline/pipeline.h"

namespace {

using mw::streamer::InputState;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;
using mw::streamer::RemuxSink;
using mw::streamer::RemuxSinkConfig;
using mw::streamer::OperationSnapshot;
using mw::streamer::PerformanceType;
using mw::streamer::PipelineSnapshot;
using mw::streamer::PacketSinkState;
using namespace mw::streamer;

void PrintPerformance(Pipeline& pipeline,
                      std::optional<PipelineSnapshot>* previous) {
  auto current = pipeline.GetPerformance();
  const auto rates = previous->has_value()
                         ? current.WithRatesSince(previous->value())
                         : current;
  const auto input = rates.input.operations.empty()
                         ? OperationSnapshot{}
                         : rates.input.operations.front();
  const auto remux_matches = rates.Find(PerformanceType::kRemux);
  const auto remux = remux_matches.empty() ? OperationSnapshot{}
                                           : *remux_matches.front().operation;
  fmt::print(
      "性能：input output={} ({:.1f} packets/s)，"
      "remux input={} ({:.1f} packets/s) output={} ({:.1f} packets/s)\n",
      input.output_count, input.output_per_second, remux.input_count,
      remux.input_per_second, remux.output_count, remux.output_per_second);
  *previous = std::move(current);
}

int Run(int argc, char* argv[]) {
  ZlmInputConfig input;
  input.url = argv[1];
  Pipeline pipeline(std::make_unique<ZlmInput>(std::move(input)));
  RemuxSinkConfig config;
  config.target = argv[2];
  auto sink = std::make_unique<RemuxSink>("remux", std::move(config));
  auto* output = sink.get();
  pipeline.AddSink(std::move(sink));
  pipeline.Start();
  std::optional<PipelineSnapshot> previous = pipeline.GetPerformance();
  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (pipeline.state() != PipelineState::kFailed &&
         pipeline.input_status().state != InputState::kFailed &&
         output->state() != PacketSinkState::kEnded &&
         output->state() != PacketSinkState::kFailed) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_report) {
      PrintPerformance(pipeline, &previous);
      next_report = now + std::chrono::seconds(1);
    }
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
  if (output->state() == PacketSinkState::kFailed) {
    fmt::print(stderr, "输出失败：{}，{}\n", argv[2], output->error());
    failed = true;
  } else {
    fmt::print("输出已关闭：{}\n", argv[2]);
  }
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    fmt::print(stderr, "用法：{} input_url push_url\n", argv[0]);
    return 2;
  }
  int result = 1;
  try {
    result = Run(argc, argv);
  } catch (const std::exception& error) {
    fmt::print(stderr, "运行失败：{}\n", error.what());
  }
  return result;
}

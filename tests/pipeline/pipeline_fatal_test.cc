#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "mw/streamer/decoder/decoder_sink.h"
#include "mw/streamer/input/zlm_input.h"
#include "mw/streamer/pipeline/pipeline.h"
#include "mw/streamer/processor/analysis_processor_sink.h"
#include "mw/streamer/processor/transform_processor_sink.h"
#include "mw/streamer/sink/fatal_error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::DecoderSink;
using mw::streamer::DecoderSinkConfig;
using mw::streamer::Input;
using mw::streamer::InputState;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;
using mw::streamer::FrameReady;
using mw::streamer::FrameStreamsReady;
using mw::streamer::PacketReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;
using mw::streamer::AnalysisProcessorSink;
using mw::streamer::TransformProcessorSink;
using mw::streamer::FatalError;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using namespace mw::streamer;

class StopProbe final {
 public:
  void Enter() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++entered_;
    changed_.notify_all();
  }

  void Complete() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++completed_;
    changed_.notify_all();
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 10s, [this]() { return completed_ != 0; });
  }

  int calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entered_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  int entered_ = 0;
  int completed_ = 0;
};

class TestInput final : public Input {
 public:
  TestInput(StopProbe& stop, bool emit_packet = false,
            bool throw_after_packet = false)
      : stop_(stop),
        emit_packet_(emit_packet),
        throw_after_packet_(throw_after_packet) {}

  void Start(Observer& observer) override {
    std::lock_guard<std::mutex> lock(delivery_mutex_);
    start_thread_ = std::this_thread::get_id();
    in_start_.store(true);
    state_.store(InputState::kReady);
    observer.OnInputStateChanged({1, InputState::kReady, "", false});
    if (emit_packet_) {
      observer.OnStreamsReady({1, {}});
      observer.OnPacket({1, {}});
    }
    in_start_.store(false);
    if (throw_after_packet_) {
      throw std::runtime_error("source start failed after delivery");
    }
  }

  void Stop() noexcept override {
    stop_.Enter();
    // Record an illegal synchronous stop instead of deadlocking this fake's
    // delivery mutex, so the test can diagnose a regression and still unwind.
    if (in_start_.load() && start_thread_ == std::this_thread::get_id()) {
      stopped_in_delivery_.store(true);
      stop_.Complete();
      return;
    }
    std::lock_guard<std::mutex> lock(delivery_mutex_);
    state_.store(InputState::kStopped);
    stop_.Complete();
  }

  InputState state() const noexcept override { return state_.load(); }
  bool stopped_in_delivery() const { return stopped_in_delivery_.load(); }

 private:
  StopProbe& stop_;
  const bool emit_packet_;
  const bool throw_after_packet_;
  std::mutex delivery_mutex_;
  std::thread::id start_thread_;
  std::atomic<bool> in_start_ = false;
  std::atomic<bool> stopped_in_delivery_ = false;
  std::atomic<InputState> state_{InputState::kIdle};
};

struct DeliveryGate {
  std::promise<void> entered;
  std::shared_future<void> release;
};

class TestPacketSink final : public Sink {
 public:
  explicit TestPacketSink(std::string id, StopProbe& stop,
                          std::string fatal_on_packet = "",
                          DeliveryGate* gate = nullptr)
      : Sink(std::move(id), SinkMediaType::kPacket),
        stop_(stop),
        fatal_on_packet_(std::move(fatal_on_packet)),
        gate_(gate) {}

  void OnStreamsReady(const StreamsReady&) noexcept override {
    state_.store(PacketSinkState::kRunning);
  }
  void OnPacket(const PacketReady&) noexcept override {
    if (!fatal_on_packet_.empty()) {
      EmitFatal(fatal_on_packet_);
    }
    if (gate_) {
      gate_->entered.set_value();
      gate_->release.wait();
    }
  }
  void OnTimelineReset(const TimelineReset&) noexcept override {}
  void OnInputEnded(const StreamEnded&) noexcept override {}
  void Stop() noexcept override {
    stop_.Enter();
    state_.store(PacketSinkState::kStopped);
    stop_.Complete();
  }
  PacketSinkState state() const noexcept { return state_.load(); }

  void EmitFatal(const std::string& error) noexcept { ReportFatalError(error); }
  void FailLocally() { state_.store(PacketSinkState::kFailed); }

 private:
  StopProbe& stop_;
  const std::string fatal_on_packet_;
  DeliveryGate* gate_;
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
};

class FrameCounter final : public Sink {
 public:
  explicit FrameCounter(StopProbe& stop)
      : Sink("frame_counter", SinkMediaType::kFrame), stop_(stop) {}
  void OnStreamsReady(const FrameStreamsReady&) override {}
  void OnAudioFrame(const FrameReady&) override {}
  void OnVideoFrame(const FrameReady&) override { videos_.fetch_add(1); }
  void OnTimelineReset(const TimelineReset&) override {}
  void OnInputEnded(const StreamEnded&) override {}
  void Stop() noexcept override {
    stop_.Enter();
    stop_.Complete();
  }
  int videos() const { return videos_.load(); }

 private:
  StopProbe& stop_;
  std::atomic<int> videos_ = 0;
};

class MessageSource final : public Sink {
 public:
  explicit MessageSource(StopProbe& stop)
      : Sink("message_source", SinkMediaType::kFrame), stop_(stop) {}
  void OnStreamsReady(const FrameStreamsReady&) override {
    StartMessages();
    SendMessage({"analytics", "ready"});
  }
  void OnAudioFrame(const FrameReady&) override {}
  void OnVideoFrame(const FrameReady&) override {}
  void OnTimelineReset(const TimelineReset&) override {}
  void OnInputEnded(const StreamEnded&) override {}
  void Stop() noexcept override {
    stop_.Enter();
    stop_.Complete();
  }

 private:
  StopProbe& stop_;
};

}  // namespace

TEST_CASE("Pipeline同步输入回调报告fatal后自动停止整条链路") {
  StopProbe input_stop;
  StopProbe failed_stop;
  StopProbe healthy_stop;
  auto input = std::make_unique<TestInput>(input_stop, true);
  const auto* source = input.get();
  Pipeline pipeline(std::move(input));
  pipeline.AddSink(std::make_unique<TestPacketSink>("failed_stop", failed_stop,
                                                    "synchronous fatal"));
  pipeline.AddSink(
      std::make_unique<TestPacketSink>("healthy_stop", healthy_stop));
  auto start = std::async(std::launch::async, [&]() { pipeline.Start(); });
  REQUIRE(start.wait_for(2s) == std::future_status::ready);
  CHECK_NOTHROW(start.get());
  REQUIRE(healthy_stop.Wait());
  REQUIRE(failed_stop.Wait());
  REQUIRE(input_stop.Wait());
  CHECK_FALSE(source->stopped_in_delivery());
  CHECK(source->state() == InputState::kStopped);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.error() == "synchronous fatal");
  CHECK(input_stop.calls() == 1);
  CHECK(failed_stop.calls() == 1);
  CHECK(healthy_stop.calls() == 1);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.error() == "synchronous fatal");
}

TEST_CASE("Pipeline合并两个异步Sink的fatal并只停止各组件一次") {
  StopProbe input_stop;
  StopProbe first_stop;
  StopProbe second_stop;
  Pipeline pipeline(std::make_unique<TestInput>(input_stop));
  auto first = std::make_unique<TestPacketSink>("first_stop", first_stop);
  auto second = std::make_unique<TestPacketSink>("second_stop", second_stop);
  auto* first_sink = first.get();
  auto* second_sink = second.get();
  pipeline.AddSink(std::move(first));
  pipeline.AddSink(std::move(second));
  pipeline.Start();
  std::promise<void> release;
  const auto gate = release.get_future().share();
  auto first_failure = std::async(std::launch::async, [&]() {
    gate.wait();
    first_sink->EmitFatal("first fatal");
  });
  auto second_failure = std::async(std::launch::async, [&]() {
    gate.wait();
    second_sink->EmitFatal("second fatal");
  });
  release.set_value();
  REQUIRE(first_failure.wait_for(2s) == std::future_status::ready);
  REQUIRE(second_failure.wait_for(2s) == std::future_status::ready);
  first_failure.get();
  second_failure.get();
  REQUIRE(first_stop.Wait());
  REQUIRE(second_stop.Wait());
  const std::string first_error = pipeline.error();
  CHECK((first_error == "first fatal" || first_error == "second fatal"));
  first_sink->EmitFatal("late first fatal");
  second_sink->EmitFatal("late second fatal");
  pipeline.Stop();
  CHECK(pipeline.error() == first_error);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(input_stop.calls() == 1);
  CHECK(first_stop.calls() == 1);
  CHECK(second_stop.calls() == 1);
}

TEST_CASE("Pipeline的fatal停机与外部Stop竞争时等待在途投递") {
  StopProbe input_stop;
  StopProbe sink_stop;
  std::promise<void> release;
  DeliveryGate gate;
  gate.release = release.get_future().share();
  auto entered = gate.entered.get_future();
  Pipeline pipeline(std::make_unique<TestInput>(input_stop, true));
  pipeline.AddSink(std::make_unique<TestPacketSink>("sink_stop", sink_stop,
                                                    "blocked fatal", &gate));
  auto start = std::async(std::launch::async, [&]() { pipeline.Start(); });
  const auto callback_entered = entered.wait_for(2s);
  std::promise<void> first_stop_entered;
  std::promise<void> second_stop_entered;
  auto first_entered = first_stop_entered.get_future();
  auto second_entered = second_stop_entered.get_future();
  auto first_stop = std::async(std::launch::async, [&]() {
    first_stop_entered.set_value();
    pipeline.Stop();
  });
  auto second_stop = std::async(std::launch::async, [&]() {
    second_stop_entered.set_value();
    pipeline.Stop();
  });
  const auto first_started = first_entered.wait_for(2s);
  const auto second_started = second_entered.wait_for(2s);
  const auto first_wait = first_stop.wait_for(20ms);
  const auto second_wait = second_stop.wait_for(20ms);
  const int sinks_stopped_while_blocked = sink_stop.calls();
  release.set_value();
  const auto start_done = start.wait_for(2s);
  const auto first_done = first_stop.wait_for(2s);
  const auto second_done = second_stop.wait_for(2s);

  REQUIRE(callback_entered == std::future_status::ready);
  REQUIRE(first_started == std::future_status::ready);
  REQUIRE(second_started == std::future_status::ready);
  CHECK(first_wait == std::future_status::timeout);
  CHECK(second_wait == std::future_status::timeout);
  CHECK(sinks_stopped_while_blocked == 0);
  REQUIRE(start_done == std::future_status::ready);
  REQUIRE(first_done == std::future_status::ready);
  REQUIRE(second_done == std::future_status::ready);
  CHECK_NOTHROW(start.get());
  CHECK_NOTHROW(first_stop.get());
  CHECK_NOTHROW(second_stop.get());
  CHECK(input_stop.calls() == 1);
  CHECK(sink_stop.calls() == 1);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.error() == "blocked fatal");
}

TEST_CASE("Sink本地失败后仍可报告一次明确fatal") {
  StopProbe stop;
  TestPacketSink sink("sink", stop);
  int reports = 0;
  std::string error;
  sink.SetOnFatalError([&](const std::string& reported) {
    ++reports;
    error = reported;
  });
  sink.FailLocally();
  sink.EmitFatal("promoted fatal");
  sink.EmitFatal("duplicate fatal");
  CHECK(reports == 1);
  CHECK(error == "promoted fatal");
}

TEST_CASE("Pipeline输入启动抛错且已报告fatal时仍完整清理") {
  StopProbe input_stop;
  StopProbe sink_stop;
  {
    Pipeline pipeline(std::make_unique<TestInput>(input_stop, true, true));
    pipeline.AddSink(std::make_unique<TestPacketSink>("sink_stop", sink_stop,
                                                      "fatal before throw"));
    CHECK_THROWS_AS(pipeline.Start(), std::exception);
    REQUIRE(input_stop.Wait());
    REQUIRE(sink_stop.Wait());
    CHECK(pipeline.state() == PipelineState::kFailed);
    CHECK(pipeline.error() == "fatal before throw");
  }
  CHECK(input_stop.calls() == 1);
  CHECK(sink_stop.calls() == 1);
}

TEST_CASE("真实Processor链路fatal自动停止Pipeline及健康旁路") {
  StopProbe healthy_stop;
  StopProbe output_stop;
  ZlmInputConfig input_config;
  input_config.url =
      std::string(MW_PIPELINE_FATAL_TEST_DATA_DIR) + "/h264_aac.mp4";
  input_config.reconnect_policy.max_retries = 0;
  auto input = std::make_unique<ZlmInput>(input_config);
  const auto* source = input.get();
  Pipeline pipeline(std::move(input));
  DecoderSinkConfig decoder_config;
  decoder_config.video_decoder.backend =
      mw::streamer::VideoDecoderBackend::kSoftware;
  auto decoder = std::make_unique<DecoderSink>("decoder", decoder_config);
  auto* decoder_sink = decoder.get();
  auto output = std::make_unique<FrameCounter>(output_stop);
  const auto* frames = output.get();
  bool output_size_failure = false;
  SECTION("Transform回调改变输出尺寸") {
    output_size_failure = true;
    MwStreamerTransformProcessorCallbacks callbacks{};
    callbacks.process_video =
        [](const MwStreamerTransformVideoProcessRequest* request, void*) {
          request->output->width = 128;
        };
    auto transform =
        std::make_unique<TransformProcessorSink>("processor", callbacks);
    transform->AddSink(std::move(output));
    decoder->AddSink(std::move(transform));
  }
  SECTION("Analysis业务回调明确抛出FatalError") {
    MwStreamerAnalysisProcessorCallbacks callbacks{};
    callbacks.process_video = [](const MwStreamerVideoFrameView*, void*) {
      throw FatalError("processor callback fatal");
    };
    decoder->AddSink(
        std::make_unique<AnalysisProcessorSink>("processor", callbacks));
    decoder->AddSink(std::move(output));
  }
  pipeline.AddSink(std::move(decoder));
  pipeline.AddSink(
      std::make_unique<TestPacketSink>("healthy_stop", healthy_stop));
  pipeline.Start();

  // These notifications come from automatic fatal shutdown. Do not manually
  // Stop first: that would hide a lost fatal notification or a control
  // deadlock.
  REQUIRE(healthy_stop.Wait());
  REQUIRE(output_stop.Wait());
  CHECK(source->state() == InputState::kStopped);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(decoder_sink->state() == PacketSinkState::kFailed);
  CHECK(frames->videos() == 0);
  if (output_size_failure) {
    CHECK(pipeline.error().find("128x1080") != std::string::npos);
    CHECK(pipeline.error().find("1920x1080") != std::string::npos);
    CHECK(decoder_sink->error().find("128x1080") != std::string::npos);
  } else {
    CHECK(pipeline.error() == "processor callback fatal");
    CHECK(decoder_sink->error() == "processor callback fatal");
  }
  CHECK(healthy_stop.calls() == 1);
  CHECK(output_stop.calls() == 1);
}

TEST_CASE("Processor异步消息fatal穿过Sink链路自动停止Pipeline") {
  StopProbe healthy_stop;
  StopProbe output_stop;
  ZlmInputConfig input_config;
  input_config.url =
      std::string(MW_PIPELINE_FATAL_TEST_DATA_DIR) + "/h264_aac.mp4";
  input_config.reconnect_policy.max_retries = 0;
  auto input = std::make_unique<ZlmInput>(input_config);
  const auto* source = input.get();
  Pipeline pipeline(std::move(input));
  DecoderSinkConfig decoder_config;
  decoder_config.video_decoder.backend =
      mw::streamer::VideoDecoderBackend::kSoftware;
  auto decoder = std::make_unique<DecoderSink>("decoder", decoder_config);
  const auto* decoder_sink = decoder.get();
  MwStreamerTransformProcessorCallbacks callbacks{};
  callbacks.on_message = [](const MwStreamerMessage*, void*) {
    throw FatalError("message callback fatal");
  };
  auto processor =
      std::make_unique<TransformProcessorSink>("processor", callbacks);
  auto message_source = std::make_unique<MessageSource>(output_stop);
  pipeline.SetMessageReceiver("message_source", "processor");
  processor->AddSink(std::move(message_source));
  SECTION("直接接入Decoder") { decoder->AddSink(std::move(processor)); }
  SECTION("经过另一层Transform转交fatal") {
    auto parent = std::make_unique<TransformProcessorSink>(
        "parent", MwStreamerTransformProcessorCallbacks{});
    parent->AddSink(std::move(processor));
    decoder->AddSink(std::move(parent));
  }
  pipeline.AddSink(std::move(decoder));
  pipeline.AddSink(
      std::make_unique<TestPacketSink>("healthy_stop", healthy_stop));
  pipeline.Start();
  REQUIRE(healthy_stop.Wait());
  REQUIRE(output_stop.Wait());
  CHECK(source->state() == InputState::kStopped);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(decoder_sink->state() == PacketSinkState::kFailed);
  CHECK(pipeline.error() == "message callback fatal");
  CHECK(decoder_sink->error() == "message callback fatal");
  CHECK(healthy_stop.calls() == 1);
  CHECK(output_stop.calls() == 1);
}

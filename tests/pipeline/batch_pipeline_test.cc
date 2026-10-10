#include "mw/streamer/pipeline/batch_pipeline.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::BatchPipeline;
using mw::streamer::BatchPipelineConfig;
using mw::streamer::InputMode;
using mw::streamer::PipelineState;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

std::string Fixture(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) + "/" + name;
}

bool WaitUntil(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

class CallbackGate final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [this] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

struct ReleaseGate final {
  CallbackGate& gate;
  ~ReleaseGate() { gate.Release(); }
};

struct StopPipeline final {
  BatchPipeline& pipeline;
  ~StopPipeline() { pipeline.Stop(); }
};

struct ReferenceTrack final {
  AVRational time_base;
  std::vector<ffmpeg::Frame> frames;
};

// Independently decode and drain both tracks so missed delayed frames, changed
// media timestamps and altered PCM are visible in the assembled pipeline.
std::map<AVMediaType, ReferenceTrack> DecodeReference(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "open batch pipeline reference");
  const auto close = [](AVFormatContext* context) {
    avformat_close_input(&context);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(raw, nullptr),
      "find batch pipeline reference streams");
  std::map<int, ffmpeg::CodecContext> decoders;
  std::map<AVMediaType, ReferenceTrack> tracks;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int index = av_find_best_stream(raw, type, -1, -1, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(index,
                                          "select batch reference track");
    const auto* stream = raw->streams[index];
    ffmpeg::CodecContext decoder(
        avcodec_find_decoder(stream->codecpar->codec_id));
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(decoder.get(), stream->codecpar),
        "configure batch reference decoder");
    decoder.get()->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr),
        "open batch reference decoder");
    decoders.emplace(index, std::move(decoder));
    tracks.emplace(type, ReferenceTrack{stream->time_base, {}});
  }
  const auto receive = [&](ffmpeg::CodecContext& decoder) {
    ffmpeg::Frame frame;
    for (;;) {
      const int status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(status, "receive batch reference");
      tracks.at(decoder.get()->codec_type).frames.push_back(frame.Ref());
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "read batch reference");
    const auto decoder = decoders.find(packet->stream_index);
    if (decoder == decoders.end()) continue;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder->second.get(), packet.get()),
        "send batch reference");
    receive(decoder->second);
  }
  for (auto& [index, decoder] : decoders) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder.get(), nullptr), "drain batch reference");
    receive(decoder);
  }
  return tracks;
}

TEST_CASE("BatchPipeline defaults to batch input at eight times speed",
          "[pipeline][batch][configuration]") {
  Runtime runtime;
  BatchPipelineConfig config;
  CHECK(config.input.mode == InputMode::kBatch);
  CHECK(config.input.playback_speed == 8.0);
  const auto mode = GENERATE(InputMode::kLive, InputMode::kRemux);
  config.input.mode = mode;
  CHECK_THROWS_AS(BatchPipeline{config}, std::invalid_argument);
  config.input.mode = InputMode::kBatch;
  config.input.playback_speed = 9.0;
  CHECK_THROWS_AS(BatchPipeline{config}, std::invalid_argument);
  config.input.playback_speed = 0.5;
  CHECK_NOTHROW(BatchPipeline{config});
  BatchPipeline pipeline;
  CHECK(pipeline.state() == PipelineState::kIdle);
  CHECK_THROWS_AS(pipeline.Start(""), std::invalid_argument);
}

TEST_CASE("BatchPipeline preserves complete decoded media and ends before stop",
          "[pipeline][batch][media][eof]") {
  Runtime runtime;
  const auto reference = DecodeReference(Fixture());
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  std::map<AVMediaType, std::vector<ffmpeg::Frame>> frames;
  std::vector<std::string> events;
  std::exception_ptr callback_error;
  std::thread::id worker;
  bool synchronous = true, device_shared = false;
  BatchPipeline pipeline(cpu);
  pipeline.SetOnReady([&](const auto& streams, const auto& device) noexcept {
    worker = std::this_thread::get_id();
    device_shared = device.get() == cpu.get();
    try {
      events.push_back("ready");
      return streams.size() == reference.size();
    } catch (...) {
      callback_error = std::current_exception();
      return false;
    }
  });
  const auto consume = [&](AVMediaType type,
                           const ffmpeg::Frame& frame) noexcept {
    synchronous &= std::this_thread::get_id() == worker;
    try {
      frames[type].push_back(frame.Ref());
    } catch (...) {
      callback_error = std::current_exception();
    }
  };
  pipeline.SetOnVideo(
      [&](const auto& frame) noexcept { consume(AVMEDIA_TYPE_VIDEO, frame); });
  pipeline.SetOnAudio(
      [&](const auto& frame) noexcept { consume(AVMEDIA_TYPE_AUDIO, frame); });
  const auto event = [&](const char* name) noexcept {
    try {
      events.emplace_back(name);
    } catch (...) {
      callback_error = std::current_exception();
    }
  };
  pipeline.SetOnEnded([&]() noexcept { event("ended"); });
  pipeline.SetOnStop([&]() noexcept { event("stop"); });
  pipeline.SetOnStateChanged([&](const auto& status) {
    if (status.state == PipelineState::kEnded) event("terminal");
  });
  pipeline.Start(Fixture());
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  pipeline.Stop();
  REQUIRE_FALSE(callback_error);
  CHECK(synchronous);
  CHECK(device_shared);
  CHECK(events ==
        std::vector<std::string>{"ready", "ended", "stop", "terminal"});
  CHECK(pipeline.state() == PipelineState::kEnded);
  CHECK(pipeline.status().error_message.empty());
  const auto ready = pipeline.ready_info();
  REQUIRE(ready.streams.size() == reference.size());
  for (const auto& stream : ready.streams) {
    const auto type = stream.codec_parameters.get()->codec_type;
    CHECK(av_cmp_q(stream.time_base, reference.at(type).time_base) == 0);
    CHECK(stream.codec_parameters.get()->sample_rate ==
          reference.at(type).frames.front()->sample_rate);
  }
  for (const auto& [type, track] : reference) {
    const auto& actual = frames.at(type);
    REQUIRE(actual.size() == track.frames.size());
    for (std::size_t position = 0; position < actual.size(); ++position) {
      const auto& frame = actual[position];
      const auto& expected = track.frames[position];
      CHECK(frame->best_effort_timestamp == expected->best_effort_timestamp);
      CHECK(frame->pts == av_rescale_q(expected->best_effort_timestamp,
                                       track.time_base, kNanoseconds));
      CHECK(av_cmp_q(frame->time_base, kNanoseconds) == 0);
      CHECK(frame->duration ==
            av_rescale_q(expected->duration, track.time_base, kNanoseconds));
      CHECK(frame->pkt_dts == AV_NOPTS_VALUE);
      REQUIRE(frame->format == expected->format);
      CHECK(frame->width == expected->width);
      CHECK(frame->height == expected->height);
      CHECK(frame->sample_rate == expected->sample_rate);
      REQUIRE(frame->nb_samples == expected->nb_samples);
      if (type != AVMEDIA_TYPE_AUDIO) continue;
      REQUIRE(av_channel_layout_compare(&frame->ch_layout,
                                        &expected->ch_layout) == 0);
      const auto sample_format = static_cast<AVSampleFormat>(expected->format);
      const bool planar = av_sample_fmt_is_planar(sample_format) != 0;
      const int planes = planar ? expected->ch_layout.nb_channels : 1;
      const int bytes = expected->nb_samples *
                        av_get_bytes_per_sample(sample_format) *
                        (planar ? 1 : expected->ch_layout.nb_channels);
      REQUIRE(bytes > 0);
      for (int plane = 0; plane < planes; ++plane) {
        CHECK(std::memcmp(frame->extended_data[plane],
                          expected->extended_data[plane],
                          static_cast<std::size_t>(bytes)) == 0);
      }
    }
  }
}

TEST_CASE("BatchPipeline synchronous Start waits for ready and first consumer",
          "[pipeline][batch][startup][control]") {
  Runtime runtime;
  CallbackGate gate;
  std::atomic<int> frames{0};
  BatchPipelineConfig config;
  config.input.loop = true;
  BatchPipeline pipeline(config);
  const bool block_ready = GENERATE(false, true);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    if (block_ready) gate.Enter();
    return true;
  });
  const auto consume = [&](const auto&) noexcept {
    if (++frames == 1 && !block_ready) gate.Enter();
  };
  pipeline.SetOnVideo(consume);
  pipeline.SetOnAudio(consume);
  auto start =
      std::async(std::launch::async, [&] { pipeline.Start(Fixture()); });
  ReleaseGate release_start{gate};
  REQUIRE(gate.Wait());
  CHECK(pipeline.state() == PipelineState::kStarting);
  CHECK(start.wait_for(100ms) == std::future_status::timeout);
  SECTION("ready and first-frame processing complete before Start returns") {
    gate.Release();
    REQUIRE(start.wait_for(5s) == std::future_status::ready);
    REQUIRE_NOTHROW(start.get());
    CHECK(frames > 0);
    CHECK(pipeline.state() == PipelineState::kRunning);
    pipeline.Stop();
  }
  SECTION("Stop waits for startup callback and aborts synchronous Start") {
    auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
    ReleaseGate release_stop{gate};
    REQUIRE(WaitUntil(
        [&] { return pipeline.state() == PipelineState::kStopping; }));
    CHECK(stop.wait_for(100ms) == std::future_status::timeout);
    gate.Release();
    REQUIRE(stop.wait_for(5s) == std::future_status::ready);
    REQUIRE_NOTHROW(stop.get());
    REQUIRE(start.wait_for(5s) == std::future_status::ready);
    try {
      start.get();
      FAIL("主动停止启动中的管线必须让 Start 报告中止");
    } catch (const ffmpeg::FfmpegException& error) {
      CHECK(error.error_code() == AVERROR_EXIT);
    }
    CHECK(pipeline.state() == PipelineState::kStopped);
    CHECK(pipeline.status().error_message.empty());
  }
}

TEST_CASE(
    "BatchPipeline Stop joins an active consumer without EOF notification",
    "[pipeline][batch][control]") {
  Runtime runtime;
  CallbackGate gate;
  std::atomic<int> frames{0}, ended{0}, stopped{0};
  BatchPipelineConfig config;
  config.input.loop = true;
  BatchPipeline pipeline(config);
  const auto consume = [&](const auto&) noexcept {
    if (++frames == 2) gate.Enter();
  };
  pipeline.SetOnVideo(consume);
  pipeline.SetOnAudio(consume);
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  // Release before pipeline destruction even if an assertion fails.
  ReleaseGate release_pipeline{gate};
  pipeline.Start(Fixture());
  REQUIRE(gate.Wait());
  auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
  ReleaseGate release_stop{gate};
  REQUIRE(
      WaitUntil([&] { return pipeline.state() == PipelineState::kStopping; }));
  CHECK(stop.wait_for(100ms) == std::future_status::timeout);
  CHECK(stopped == 0);
  gate.Release();
  REQUIRE(stop.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(stop.get());
  const int completed = frames.load();
  pipeline.Stop();
  std::this_thread::sleep_for(30ms);
  CHECK(frames == completed);
  CHECK(ended == 0);
  CHECK(stopped == 1);
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE(
    "BatchPipeline looping retains callbacks and exposes original media time",
    "[pipeline][batch][loop][restart]") {
  Runtime runtime;
  std::atomic<int> ready{0}, ended{0}, stopped{0}, loops{0};
  std::int64_t previous = AV_NOPTS_VALUE;
  BatchPipelineConfig config;
  config.input.loop = true;
  config.input.playback_speed = 4.0;
  BatchPipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    previous = AV_NOPTS_VALUE;
    return true;
  });
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    if (previous != AV_NOPTS_VALUE && frame->pts < previous) ++loops;
    previous = frame->pts;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  for (int session = 0; session < 2; ++session) {
    const int previous_loops = loops.load();
    pipeline.Start(Fixture());
    CHECK_THROWS_AS(pipeline.Start(Fixture()), std::logic_error);
    REQUIRE(WaitUntil([&] { return loops > previous_loops; }));
    CHECK(pipeline.state() == PipelineState::kRunning);
    CHECK(ready == session + 1);
    CHECK(ended == 0);
    pipeline.Stop();
    CHECK(pipeline.state() == PipelineState::kStopped);
    CHECK(stopped == session + 1);
  }
}

TEST_CASE("BatchPipeline rejected ready and failed input clean up and recover",
          "[pipeline][batch][failure][restart]") {
  Runtime runtime;
  std::atomic<int> ready{0}, frames{0}, ended{0}, stopped{0};
  bool accepted = false;
  BatchPipeline pipeline;
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return accepted;
  });
  const auto consume = [&](const auto&) noexcept { ++frames; };
  pipeline.SetOnVideo(consume);
  pipeline.SetOnAudio(consume);
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  SECTION("Input fails before preparation") {
    CHECK_THROWS_AS(pipeline.Start(Fixture("missing-batch-pipeline.mp4")),
                    ffmpeg::FfmpegException);
    CHECK(ready == 0);
  }
  SECTION("Processor refuses preparation") {
    CHECK_THROWS(pipeline.Start(Fixture()));
    CHECK(ready == 1);
  }
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK_FALSE(pipeline.status().error_source.empty());
  CHECK_FALSE(pipeline.status().error_message.empty());
  CHECK(frames == 0);
  CHECK(ended == 0);
  CHECK(stopped == 0);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
  accepted = true;
  pipeline.Start(Fixture());
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(frames > 0);
  CHECK(ended == 1);
  CHECK(stopped == 1);
  CHECK(pipeline.status().error_source.empty());
  CHECK(pipeline.status().error_message.empty());
  CHECK(pipeline.status().error_code == 0);
}

TEST_CASE("BatchPipeline Stop ends retry wait and aborts synchronous Start",
          "[pipeline][batch][startup][retry]") {
  Runtime runtime;
  std::atomic<int> ready{0}, ended{0}, stopped{0};
  BatchPipelineConfig config;
  config.input.open_timeout = 250ms;
  config.input.retry_interval = 30s;
  config.input.max_retries = -1;
  BatchPipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return true;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  auto start = std::async(std::launch::async, [&] {
    pipeline.Start("rtsp://127.0.0.1:1/live/unavailable");
  });
  StopPipeline stop_on_failure{pipeline};
  REQUIRE(WaitUntil(
      [&] { return pipeline.state() == PipelineState::kReconnecting; }));
  CHECK(start.wait_for(0ms) == std::future_status::timeout);
  auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
  REQUIRE(stop.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(stop.get());
  REQUIRE(start.wait_for(5s) == std::future_status::ready);
  try {
    start.get();
    FAIL("重连等待被主动停止后 Start 必须报告中止");
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_EXIT);
  }
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK(ready == 0);
  CHECK(ended == 0);
  CHECK(stopped == 0);
  CHECK(pipeline.status().error_message.empty());
}

TEST_CASE(
    "BatchPipeline catches state observer failures and skips unset consumers",
    "[pipeline][batch][callbacks]") {
  Runtime runtime;
  std::atomic<int> ended{0}, stopped{0}, terminal{0};
  BatchPipeline pipeline;
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  pipeline.SetOnStateChanged([&](const auto& status) {
    if (status.state == PipelineState::kEnded) ++terminal;
    throw std::runtime_error("测试状态通知异常");
  });
  REQUIRE_NOTHROW(pipeline.Start(Fixture()));
  REQUIRE(WaitUntil([&] { return terminal == 1; }));
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kEnded);
  CHECK(ended == 1);
  CHECK(stopped == 1);
}

}  // namespace

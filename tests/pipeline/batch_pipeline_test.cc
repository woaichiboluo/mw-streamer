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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "multitrack_fixture.h"

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

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
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
  CHECK(config.playback_speed == 8.0);
  const auto mode = GENERATE(InputMode::kLive, InputMode::kRemux);
  config.input.mode = mode;
  CHECK_THROWS_AS(BatchPipeline{config}, std::invalid_argument);
  config.input.mode = InputMode::kBatch;
  config.playback_speed =
      GENERATE(0.0, 9.0, std::numeric_limits<double>::infinity(),
               std::numeric_limits<double>::quiet_NaN());
  CHECK_THROWS_AS(BatchPipeline{config}, std::invalid_argument);
  config.playback_speed = 0.5;
  CHECK_NOTHROW(BatchPipeline{config});
  BatchPipeline pipeline;
  CHECK(pipeline.state() == PipelineState::kIdle);
  CHECK_THROWS_AS(pipeline.Start(""), std::invalid_argument);
}

TEST_CASE("BatchPipeline limits delivery using original stream timestamps",
          "[pipeline][batch][pacing]") {
  Runtime runtime;
  BatchPipelineConfig config;
  config.playback_speed = GENERATE(1.0, 8.0);
  BatchPipeline pipeline(config);
  struct Delivery {
    std::chrono::steady_clock::time_point first, last;
    std::int64_t first_pts = AV_NOPTS_VALUE, last_pts = AV_NOPTS_VALUE;
    std::int64_t end_pts = AV_NOPTS_VALUE;
    AVRational time_base{0, 1};
  };
  Delivery video, audio;
  const auto consume = [&](Delivery& delivery, const auto& frame) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (delivery.first_pts == AV_NOPTS_VALUE) {
      delivery.first_pts = frame->pts;
      delivery.first = now;
      delivery.time_base = frame->time_base;
    }
    delivery.last_pts = frame->pts;
    delivery.end_pts = frame->pts + frame->duration;
    delivery.last = now;
  };
  pipeline.SetOnVideo(
      [&](const auto& frame) noexcept { consume(video, frame); });
  pipeline.SetOnAudio(
      [&](const auto& frame) noexcept { consume(audio, frame); });
  pipeline.Start(Fixture());
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  const auto completed = std::chrono::steady_clock::now();
  for (const auto& delivery : {video, audio}) {
    REQUIRE(delivery.first_pts != AV_NOPTS_VALUE);
    REQUIRE(delivery.last_pts > delivery.first_pts);
    const double media_seconds =
        static_cast<double>(delivery.last_pts - delivery.first_pts) *
        av_q2d(delivery.time_base);
    const double delivery_seconds =
        std::chrono::duration<double>(delivery.last - delivery.first).count();
    CHECK(delivery_seconds >= media_seconds / config.playback_speed - 0.03);
    CHECK(delivery_seconds < media_seconds / config.playback_speed + 1.0);
    const double ended_seconds =
        std::chrono::duration<double>(completed - delivery.first).count();
    const double duration_seconds =
        static_cast<double>(delivery.end_pts - delivery.first_pts) *
        av_q2d(delivery.time_base);
    CHECK(ended_seconds >= duration_seconds / config.playback_speed - 0.03);
  }
}

TEST_CASE("BatchPipeline Stop interrupts its own delivery wait",
          "[pipeline][batch][pacing][control]") {
  Runtime runtime;
  BatchPipelineConfig config;
  config.playback_speed = 0.5;
  config.input.loop = true;
  BatchPipeline pipeline(config);
  std::atomic<int> frames{0}, ended{0};
  pipeline.SetOnVideo([&](const auto&) noexcept { ++frames; });
  pipeline.SetOnAudio([&](const auto&) noexcept { ++frames; });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.Start(Fixture());
  REQUIRE(frames > 0);
  const auto stopping = std::chrono::steady_clock::now();
  pipeline.Stop();
  CHECK(std::chrono::steady_clock::now() - stopping < 250ms);
  CHECK(ended == 0);
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("BatchPipeline single-frame loop waits for its tail and ends",
          "[pipeline][batch][pacing][loop][eof]") {
  Runtime runtime;
  BatchPipelineConfig config;
  config.input.loop = true;
  BatchPipeline pipeline(config);
  std::atomic<int> frames{0}, ready{0}, ended{0}, stopped{0};
  std::atomic<int> sequence{0}, end_order{0}, stop_order{0};
  std::chrono::steady_clock::time_point delivery, end_time;
  pipeline.SetOnReady([&](const auto& streams, const auto&) noexcept {
    ++ready;
    return streams.size() == 1;
  });
  pipeline.SetOnVideo([&](const auto&) noexcept {
    delivery = std::chrono::steady_clock::now();
    ++frames;
  });
  pipeline.SetOnEnded([&]() noexcept {
    end_time = std::chrono::steady_clock::now();
    end_order = ++sequence;
    ++ended;
  });
  pipeline.SetOnStop([&]() noexcept {
    stop_order = ++sequence;
    ++stopped;
  });
  StopPipeline stop_on_failure{pipeline};
  pipeline.Start(Fixture("seek_tail_h264.mp4"));
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  REQUIRE(frames == 1);
  CHECK(std::chrono::duration<double>(end_time - delivery).count() >=
        2.0 / config.playback_speed - 0.02);
  CHECK(ready == 1);
  CHECK(ended == 1);
  CHECK(stopped == 1);
  CHECK(end_order == 1);
  CHECK(stop_order == 2);
}

TEST_CASE(
    "BatchPipeline multi-frame loop preserves frames and eightfold pacing",
    "[pipeline][batch][pacing][loop]") {
  Runtime runtime;
  const auto reference = DecodeReference(Fixture());
  const auto& video = reference.at(AVMEDIA_TYPE_VIDEO);
  REQUIRE(video.frames.size() > 2);
  BatchPipelineConfig config;
  config.input.loop = true;
  CHECK(config.playback_speed == 8.0);
  BatchPipeline pipeline(config);
  std::atomic<int> frames{0}, ready{0}, ended{0}, stopped{0};
  struct Delivery {
    std::chrono::steady_clock::time_point time;
    ffmpeg::Frame frame;
  };
  std::vector<Delivery> deliveries;
  std::exception_ptr callback_error;
  pipeline.SetOnReady([&](const auto& streams, const auto&) noexcept {
    ++ready;
    return streams.size() == reference.size();
  });
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    try {
      deliveries.push_back({std::chrono::steady_clock::now(), frame.Ref()});
    } catch (...) {
      callback_error = std::current_exception();
    }
    ++frames;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  StopPipeline stop_on_failure{pipeline};
  pipeline.Start(Fixture());
  const auto minimum = video.frames.size() * 2 + 1;
  REQUIRE(WaitUntil(
      [&] { return static_cast<std::size_t>(frames.load()) >= minimum; }));
  pipeline.Stop();
  REQUIRE_FALSE(callback_error);
  REQUIRE(deliveries.size() >= minimum);
  for (std::size_t index = 0; index < deliveries.size(); ++index) {
    const auto& actual = deliveries[index].frame;
    const auto& expected = video.frames[index % video.frames.size()];
    CHECK(actual->pts == expected->best_effort_timestamp);
    CHECK(actual->duration == expected->duration);
    CHECK(av_cmp_q(actual->time_base, video.time_base) == 0);
  }
  const double media_duration =
      static_cast<double>(video.frames.back()->best_effort_timestamp +
                          video.frames.back()->duration -
                          video.frames.front()->best_effort_timestamp) *
      av_q2d(video.time_base);
  for (std::size_t cycle = 1; cycle <= 2; ++cycle) {
    const auto first = cycle * video.frames.size();
    const double wall = std::chrono::duration<double>(
                            deliveries[first].time -
                            deliveries[first - video.frames.size()].time)
                            .count();
    CHECK(wall >= media_duration / config.playback_speed - 0.02);
    const double tail_wall =
        std::chrono::duration<double>(deliveries[first].time -
                                      deliveries[first - 1].time)
            .count();
    const double tail_media =
        static_cast<double>(video.frames.back()->duration) *
        av_q2d(video.time_base);
    CHECK(tail_wall >= tail_media / config.playback_speed - 0.01);
  }
  CHECK(ready >= 3);
  CHECK(ended == 0);
  CHECK(stopped == 1);
}

TEST_CASE("BatchPipeline preserves complete decoded media and ends before stop",
          "[pipeline][batch][media][eof][backpressure]") {
  Runtime runtime;
  const auto reference = DecodeReference(Fixture());
  const bool slow_consumer = GENERATE(false, true);
  CAPTURE(slow_consumer);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  std::map<AVMediaType, std::vector<ffmpeg::Frame>> frames;
  std::map<AVMediaType, std::size_t> ended_frames;
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
      // Slower than eight-times playback, forcing decoding to wait for the
      // synchronous consumer instead of dropping queued or delayed frames.
      if (slow_consumer) std::this_thread::sleep_for(20ms);
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
  pipeline.SetOnEnded([&]() noexcept {
    try {
      for (const auto& [type, delivered] : frames) {
        ended_frames.emplace(type, delivered.size());
      }
    } catch (...) {
      callback_error = std::current_exception();
    }
    event("ended");
  });
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
    REQUIRE(track.frames.size() > 8);
    REQUIRE(ended_frames.at(type) == track.frames.size());
    REQUIRE(actual.size() == track.frames.size());
    for (std::size_t position = 0; position < actual.size(); ++position) {
      const auto& frame = actual[position];
      const auto& expected = track.frames[position];
      CHECK(frame->best_effort_timestamp == expected->best_effort_timestamp);
      CHECK(frame->pts == expected->best_effort_timestamp);
      CHECK(av_cmp_q(frame->time_base, track.time_base) == 0);
      CHECK(frame->duration == expected->duration);
      CHECK(frame->pkt_dts == expected->pkt_dts);
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
  // Keep the delivery callback blocked while local reading and decoding can
  // prefetch. Stop must release upstream waits without invoking the consumer
  // for frames that were only prefetched.
  std::this_thread::sleep_for(200ms);
  CHECK(frames == 2);
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
  CHECK(completed == 2);
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
  config.playback_speed = 4.0;
  BatchPipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
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
    const int previous_ready = ready.load();
    previous = AV_NOPTS_VALUE;
    pipeline.Start(Fixture());
    CHECK_THROWS_AS(pipeline.Start(Fixture()), std::logic_error);
    REQUIRE(WaitUntil([&] { return loops > previous_loops; }));
    CHECK(pipeline.state() == PipelineState::kRunning);
    CHECK(ready >= previous_ready + 2);
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

TEST_CASE("BatchPipeline defaults to the first video and audio in source order",
          "[pipeline][batch][tracks][configuration]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false, true);
  auto config = BatchPipelineConfig{};
  config.input.loop = true;
  std::atomic<int> selected{0};
  BatchPipeline pipeline(config);
  const bool reset_callback = GENERATE(false, true);
  if (reset_callback) {
    pipeline.SetOnSelectStreams([&](const auto&) {
      ++selected;
      return std::vector<int>{3, 2};
    });
    pipeline.SetOnSelectStreams({});
  }
  pipeline.Start(source.path());
  CHECK(pipeline.state() == PipelineState::kRunning);
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  CHECK(info.streams[0].stream_index == 0);
  CHECK(info.streams[1].stream_index == 1);
  CHECK((info.streams[0].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK((info.streams[1].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK(selected == 0);
  CHECK_THROWS_AS(pipeline.SetOnSelectStreams({}), std::logic_error);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("BatchPipeline rejects selection before downstream preparation",
          "[pipeline][batch][tracks][configuration][failure]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false);
  std::atomic<int> selected{0}, ready{0}, frames{0};
  std::vector<int> offered;
  BatchPipeline pipeline(BatchPipelineConfig{});
  const bool throws = GENERATE(false, true);
  pipeline.SetOnSelectStreams([&](const auto& streams) -> std::vector<int> {
    ++selected;
    for (const auto& stream : streams) offered.push_back(stream.stream_index);
    if (throws) throw std::invalid_argument("selector rejected tracks");
    return {0, 123};
  });
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return true;
  });
  pipeline.SetOnVideo([&](const auto&) noexcept { ++frames; });
  pipeline.SetOnAudio([&](const auto&) noexcept { ++frames; });
  if (throws) {
    try {
      pipeline.Start(source.path());
      FAIL("selector exception must propagate through synchronous Start");
    } catch (const std::invalid_argument& error) {
      CHECK(std::string(error.what()) == "selector rejected tracks");
    }
  } else {
    CHECK_THROWS_AS(pipeline.Start(source.path()), ffmpeg::FfmpegException);
  }
  CHECK(selected == 1);
  CHECK(offered == std::vector<int>{0, 1, 2});
  CHECK(ready == 0);
  CHECK(frames == 0);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.status().error_source == "input");
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
}

TEST_CASE(
    "BatchPipeline uses its selector before consuming the chosen audio track",
    "[pipeline][batch][tracks][eof][control]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture());
  auto config = BatchPipelineConfig{};
  const bool stop_actively = GENERATE(false, true);
  config.input.loop = stop_actively;
  std::atomic<int> selections{0};
  std::atomic<bool> all_tracks_offered{true}, selection_finished{false};
  std::atomic<bool> selection_before_consumption{true};
  BatchPipeline pipeline(config);
  pipeline.SetOnSelectStreams([&](const auto& streams) {
    if (streams.size() != 3 || streams[0].stream_index != 0 ||
        streams[1].stream_index != 1 || streams[2].stream_index != 2) {
      all_tracks_offered = false;
    }
    ++selections;
    selection_finished = true;
    return std::vector<int>{0, 2};
  });
  std::atomic<int> videos{0}, audios{0}, ended{0}, stopped{0};
  pipeline.SetOnVideo([&](const auto&) noexcept {
    if (!selection_finished) selection_before_consumption = false;
    ++videos;
  });
  pipeline.SetOnAudio([&](const auto&) noexcept {
    if (!selection_finished) selection_before_consumption = false;
    ++audios;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  StopPipeline stop_on_failure{pipeline};
  pipeline.Start(source.path());
  if (stop_actively)
    REQUIRE(WaitUntil([&] { return videos > 0 && audios > 0; }));
  if (!stop_actively)
    REQUIRE(
        WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(pipeline.state() ==
        (stop_actively ? PipelineState::kStopped : PipelineState::kEnded));
  CHECK(videos > 0);
  CHECK(audios > 0);
  CHECK(ended == (stop_actively ? 0 : 1));
  CHECK(stopped == 1);
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  CHECK(info.streams[0].stream_index == 0);
  CHECK(info.streams[1].stream_index == 2);
  CHECK((info.streams[1].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK(selections >= 1);
  CHECK(all_tracks_offered);
  CHECK(selection_before_consumption);
}

}  // namespace

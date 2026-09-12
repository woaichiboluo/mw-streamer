#include "mw/streamer/synchronizer/synchronizer_sink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::Frame;
using mw::streamer::StreamInfo;
using mw::streamer::FrameReady;
using mw::streamer::FrameStreamsReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::TimelineReset;
using mw::streamer::TimelineResetReason;
using mw::streamer::FatalError;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using mw::streamer::SinkMessage;
using mw::streamer::SynchronizerSink;
using mw::streamer::SynchronizerSinkConfig;
using mw::streamer::SynchronizerSinkState;
using Clock = std::chrono::steady_clock;

SynchronizerSinkConfig Config() {
  SynchronizerSinkConfig config;
  config.max_frame_lateness = 60ms;
  config.standby_timeout = 100ms;
  return config;
}

FrameStreamsReady Streams(std::uint64_t generation = 1, bool audio = true,
                          bool video = true,
                          AVRational video_frame_rate = {20, 1}) {
  FrameStreamsReady ready{generation, {}, nullptr};
  if (video) {
    StreamInfo stream;
    stream.stream_index = 0;
    stream.time_base = {1, 90000};
    stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_H264;
    stream.codec_parameters.get()->width = 64;
    stream.codec_parameters.get()->height = 64;
    stream.codec_parameters.get()->framerate = video_frame_rate;
    ready.source_streams.push_back(std::move(stream));
  }
  if (audio) {
    StreamInfo stream;
    stream.stream_index = 1;
    stream.time_base = {1, 48000};
    stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_AAC;
    stream.codec_parameters.get()->sample_rate = 48000;
    av_channel_layout_default(&stream.codec_parameters.get()->ch_layout, 2);
    ready.source_streams.push_back(std::move(stream));
  }
  return ready;
}

Frame Video(std::int64_t pts, std::uint8_t marker = 29) {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 64;
  frame->height = 64;
  frame->pts = pts;
  frame->duration = 50;
  frame->time_base = {1, 1000};
  frame->color_range = AVCOL_RANGE_MPEG;
  frame->colorspace = AVCOL_SPC_BT709;
  if (av_frame_get_buffer(frame.get(), 32) < 0) {
    throw std::runtime_error("allocate video frame failed");
  }
  for (int plane = 0; plane < 3; ++plane) {
    std::memset(frame->data[plane], plane == 0 ? marker : 128,
                frame->linesize[plane] * (plane == 0 ? 64 : 32));
  }
  return frame;
}

Frame Audio(std::int64_t pts) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 960;
  frame->pts = pts;
  frame->duration = 960;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("allocate audio frame failed");
  }
  auto* samples = reinterpret_cast<float*>(frame->data[0]);
  for (int index = 0; index < 1920; ++index) samples[index] = 0.25F;
  return frame;
}

bool HasMarker(const Frame& frame, std::uint8_t marker) {
  for (int y = 0; y < frame->height; ++y) {
    for (int x = 0; x < frame->width; ++x) {
      if (frame->data[0][y * frame->linesize[0] + x] != marker) return false;
    }
  }
  return true;
}

struct Recorded {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<FrameReady> audio;
  std::vector<FrameReady> video;
  std::vector<Clock::time_point> video_times;
  std::vector<FrameStreamsReady> streams;
  std::vector<TimelineReset> resets;
  std::vector<StreamEnded> ends;
  std::vector<std::string> events;
  std::atomic<int> active{0};
  std::atomic<bool> overlapped{false};
  bool block_video = false;
  bool video_entered = false;
  bool released = false;
  bool throw_video = false;
  bool fatal_video = false;
  bool throw_ready = false;
  bool throw_end = false;
  bool fatal_end = false;
  std::chrono::milliseconds video_delay{0};
  std::thread::id callback_thread;
  int stops = 0;

  template <typename Predicate>
  bool Wait(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, predicate);
  }
  bool WaitVideos(std::size_t count) {
    return Wait([&] { return video.size() >= count; });
  }
  bool WaitMarker(std::uint8_t marker) {
    return Wait([&] {
      for (const auto& item : video) {
        if (HasMarker(item.frame, marker)) return true;
      }
      return false;
    });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

struct ReleaseOnExit {
  Recorded& recorded;
  ~ReleaseOnExit() { recorded.Release(); }
};

class Recorder final : public Sink {
 public:
  explicit Recorder(Recorded& recorded)
      : Sink("recorder", SinkMediaType::kFrame), recorded_(recorded) {}

  void OnStreamsReady(const FrameStreamsReady& streams) override {
    StartMessages();
    Enter();
    std::lock_guard<std::mutex> lock(recorded_.mutex);
    recorded_.streams.push_back(streams);
    recorded_.events.emplace_back("ready");
    Leave();
    if (recorded_.throw_ready)
      throw std::range_error("synchronizer ready failed");
  }
  void OnAudioFrame(const FrameReady& frame) override { Record(frame, true); }
  void OnVideoFrame(const FrameReady& frame) override { Record(frame, false); }
  void OnTimelineReset(const TimelineReset& reset) override {
    Enter();
    std::lock_guard<std::mutex> lock(recorded_.mutex);
    recorded_.resets.push_back(reset);
    recorded_.events.emplace_back("reset");
    Leave();
  }
  void OnInputEnded(const StreamEnded& end) override {
    Enter();
    std::lock_guard<std::mutex> lock(recorded_.mutex);
    recorded_.ends.push_back(end);
    recorded_.events.emplace_back("end");
    Leave();
    if (recorded_.fatal_end) throw FatalError("synchronizer end fatal");
    if (recorded_.throw_end) throw std::range_error("synchronizer end failed");
  }
  void Stop() noexcept override {
    Enter();
    std::lock_guard<std::mutex> lock(recorded_.mutex);
    ++recorded_.stops;
    recorded_.events.emplace_back("stop");
    Leave();
  }
  void Fatal(const std::string& message) { ReportFatalError(message); }
  void Message(const SinkMessage& message) { SendMessage(message); }

 private:
  void Enter() {
    if (recorded_.active.fetch_add(1) != 0) recorded_.overlapped.store(true);
  }
  void Leave() {
    recorded_.active.fetch_sub(1);
    recorded_.changed.notify_all();
  }
  void Record(const FrameReady& frame, bool audio) {
    Enter();
    std::unique_lock<std::mutex> lock(recorded_.mutex);
    recorded_.callback_thread = std::this_thread::get_id();
    if (!audio && (recorded_.throw_video || recorded_.fatal_video)) {
      Leave();
      if (recorded_.fatal_video) throw FatalError("synchronizer worker fatal");
      throw std::range_error("synchronizer output failed");
    }
    (audio ? recorded_.audio : recorded_.video).push_back(frame);
    recorded_.events.emplace_back(audio ? "audio" : "video");
    if (!audio) {
      recorded_.video_times.push_back(Clock::now());
      recorded_.video_entered = true;
      recorded_.changed.notify_all();
      if (recorded_.block_video) {
        recorded_.changed.wait(lock, [&] { return recorded_.released; });
      }
      if (recorded_.video_delay > 0ms) {
        const auto delay = recorded_.video_delay;
        lock.unlock();
        std::this_thread::sleep_for(delay);
        lock.lock();
      }
    }
    Leave();
  }
  Recorded& recorded_;
};

Recorder& AddRecorder(SynchronizerSink& sink, Recorded& recorded) {
  auto recorder = std::make_unique<Recorder>(recorded);
  auto& result = *recorder;
  sink.AddSink(std::move(recorder));
  return result;
}

bool WaitState(const SynchronizerSink& sink, SynchronizerSinkState state) {
  const auto deadline = Clock::now() + 3s;
  while (sink.state() != state && Clock::now() < deadline) {
    if (sink.state() == SynchronizerSinkState::kFailed) {
      return state == SynchronizerSinkState::kFailed;
    }
    std::this_thread::yield();
  }
  return sink.state() == state;
}

void CheckClocks(const Recorded& recorded, std::uint64_t generation = 1) {
  for (std::size_t index = 0; index < recorded.video.size(); ++index) {
    const auto& item = recorded.video[index];
    CHECK(item.generation == generation);
    CHECK(item.frame->time_base.num == 1);
    CHECK(item.frame->time_base.den == 20);
    CHECK(item.frame->pts == static_cast<std::int64_t>(index));
    CHECK(item.frame->duration == 1);
  }
  std::int64_t samples = 0;
  for (const auto& item : recorded.audio) {
    CHECK(item.generation == generation);
    CHECK(item.frame->time_base.num == 1);
    CHECK(item.frame->time_base.den == 48000);
    CHECK(item.frame->pts == samples);
    samples += item.frame->nb_samples;
  }
  CHECK_FALSE(recorded.overlapped.load());
}

class StopReporter final : public Sink {
 public:
  StopReporter() : Sink("reporter", SinkMediaType::kFrame) {}
  std::promise<void> ready;
  void OnStreamsReady(const FrameStreamsReady&) override {
    StartMessages();
    ready.set_value();
  }
  void OnAudioFrame(const FrameReady&) override {}
  void OnVideoFrame(const FrameReady&) override {}
  void OnTimelineReset(const TimelineReset&) override {}
  void OnInputEnded(const StreamEnded&) override {}
  void Stop() noexcept override {
    std::thread worker([this] {
      SendMessage({"worker", "stopping", nullptr, 0, std::nullopt});
      ReportFatalError("fatal while stopping");
    });
    worker.join();
    Sink::Stop();
  }
};

}  // namespace

TEST_CASE(
    "SynchronizerSink waits for both prototypes then clocks frames and "
    "silence") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams());
  sink.OnVideoFrame({1, Video(0)});
  const bool premature =
      recorded.Wait([&] { return !recorded.video.empty(); }, 80ms);
  sink.OnAudioFrame({1, Audio(0)});
  const bool played = recorded.WaitVideos(6);
  sink.Stop();
  INFO(sink.error());
  CHECK_FALSE(premature);
  REQUIRE(played);
  REQUIRE(recorded.streams.size() == 1);
  REQUIRE(recorded.audio.size() >= 2);
  REQUIRE(recorded.video.size() >= 6);
  CHECK(recorded.events.front() == "ready");
  CHECK(recorded.callback_thread != std::this_thread::get_id());
  CHECK(recorded.video_times[5] - recorded.video_times[0] >= 180ms);
  bool silence = false;
  for (const auto& item : recorded.audio) {
    const auto* samples = reinterpret_cast<const float*>(item.frame->data[0]);
    bool zero = true;
    for (int index = 0; index < item.frame->nb_samples * 2; ++index) {
      zero = zero && samples[index] == 0.0F;
    }
    silence = silence || zero;
  }
  CHECK(silence);
  CheckClocks(recorded);
  const auto snapshot = sink.GetPerformance();
  REQUIRE(snapshot.operations.size() == 1);
  REQUIRE(snapshot.downstream.size() == 1);
  const auto& operation = snapshot.operations.front();
  CHECK(operation.type ==
        mw::streamer::PerformanceType::kSynchronizer);
  CHECK(operation.input_count == 2);
  CHECK(operation.output_count >=
        recorded.audio.size() + recorded.video.size());
  CHECK(operation.completed_calls >=
        operation.input_count + operation.output_count);
  CHECK(operation.in_flight == 0);
  CHECK(operation.failed_calls == 0);
  CHECK(sink.GetPerformance().operations.front().output_count ==
        operation.output_count);
}

TEST_CASE(
    "SynchronizerSink repeats video then generates standby without input") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true));
  auto original = Video(1000);
  sink.OnVideoFrame({1, original});
  const bool played = recorded.WaitVideos(7);
  const bool standby = WaitState(sink, SynchronizerSinkState::kStandby);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(played);
  CHECK(standby);
  REQUIRE(recorded.video.size() >= 7);
  CHECK(HasMarker(recorded.video.front().frame, 29));
  CHECK(HasMarker(recorded.video[1].frame, 29));
  CHECK_FALSE(HasMarker(recorded.video.back().frame, 29));
  CHECK(original->pts == 1000);
  CHECK(recorded.audio.empty());
  CheckClocks(recorded);
}

TEST_CASE(
    "SynchronizerSink rejects late same-generation media and accepts fresh "
    "recovery") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  const bool played = recorded.WaitVideos(6);
  sink.OnVideoFrame({1, Video(40, 113)});
  const bool continued = recorded.WaitVideos(8);
  std::int64_t fresh_pts;
  {
    std::lock_guard<std::mutex> lock(recorded.mutex);
    fresh_pts = recorded.video.empty()
                    ? 500
                    : (recorded.video.back().frame->pts + 2) * 50;
  }
  sink.OnVideoFrame({1, Video(fresh_pts, 211)});
  const bool recovered = recorded.WaitMarker(211);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(played);
  REQUIRE(continued);
  REQUIRE(recovered);
  for (const auto& frame : recorded.video)
    CHECK_FALSE(HasMarker(frame.frame, 113));
  CHECK(recorded.resets.empty());
  CHECK(recorded.streams.size() == 1);
  CheckClocks(recorded);
}

TEST_CASE(
    "SynchronizerSink absorbs interruption and resets into a continuous output "
    "generation") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams());
  sink.OnAudioFrame({1, Audio(0)});
  sink.OnVideoFrame({1, Video(0)});
  const bool initial = recorded.WaitVideos(3);
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  const bool standby_continued = recorded.WaitVideos(6);
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  sink.OnStreamsReady(Streams(2));
  sink.OnAudioFrame({2, Audio(480000)});
  sink.OnVideoFrame({2, Video(10000, 211)});
  const bool recovered = recorded.WaitMarker(211);
  sink.OnInputEnded({2, StreamEndReason::kEof});
  const bool ended = recorded.Wait([&] { return !recorded.ends.empty(); });
  sink.Stop();
  INFO(sink.error());
  REQUIRE(initial);
  REQUIRE(standby_continued);
  REQUIRE(recovered);
  REQUIRE(ended);
  REQUIRE(recorded.ends.size() == 1);
  CHECK(recorded.ends.front().generation == 1);
  CHECK(recorded.ends.front().reason == StreamEndReason::kEof);
  CHECK(recorded.resets.empty());
  CHECK(recorded.streams.size() == 1);
  CheckClocks(recorded);
}

TEST_CASE("SynchronizerSink EOF drains scheduled tail at playback pace once") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true));
  for (int index = 0; index < 5; ++index) {
    sink.OnVideoFrame(
        {1, Video(index * 50, static_cast<std::uint8_t>(40 + index))});
  }
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool ended = WaitState(sink, SynchronizerSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  REQUIRE(recorded.ends.size() == 1);
  REQUIRE(recorded.video.size() == 5);
  CHECK(recorded.video_times.back() - recorded.video_times.front() >= 140ms);
  for (std::size_t index = 0; index < recorded.video.size(); ++index) {
    CHECK(HasMarker(recorded.video[index].frame,
                    static_cast<std::uint8_t>(40 + index)));
  }
  CHECK(recorded.events[recorded.events.size() - 2] == "end");
  CheckClocks(recorded);
}

TEST_CASE(
    "SynchronizerSink queue saturation discards older frames without waiting "
    "for a child") {
  Recorded recorded;
  recorded.block_video = true;
  auto config = Config();
  config.frame_queue_capacity = 2;
  SynchronizerSink sink("synchronizer", config);
  ReleaseOnExit release{recorded};
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams());
  sink.OnAudioFrame({1, Audio(0)});
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recorded.Wait([&] { return recorded.video_entered; });
  const auto start = Clock::now();
  std::exception_ptr audio_error;
  std::exception_ptr video_error;
  std::promise<void> audio_done;
  std::promise<void> video_done;
  auto audio_finished = audio_done.get_future();
  auto video_finished = video_done.get_future();
  std::thread audio([&] {
    try {
      for (int index = 1; index < 200; ++index)
        sink.OnAudioFrame({1, Audio(index * 960)});
    } catch (...) {
      audio_error = std::current_exception();
    }
    audio_done.set_value();
  });
  std::thread video([&] {
    try {
      for (int index = 1; index < 200; ++index)
        sink.OnVideoFrame({1, Video(index * 50)});
    } catch (...) {
      video_error = std::current_exception();
    }
    video_done.set_value();
  });
  const bool audio_returned =
      audio_finished.wait_for(1s) == std::future_status::ready;
  const bool video_returned =
      video_finished.wait_for(1s) == std::future_status::ready;
  const auto elapsed = Clock::now() - start;
  const auto depth = sink.queue_depth();
  recorded.Release();
  audio.join();
  video.join();
  sink.Stop();
  REQUIRE(entered);
  CHECK(audio_returned);
  CHECK(video_returned);
  CHECK_FALSE(audio_error);
  CHECK_FALSE(video_error);
  CHECK(elapsed < 1s);
  CHECK(depth <= 4 * config.frame_queue_capacity);
  CHECK(recorded.stops == 1);
  CHECK(sink.error().empty());
  CHECK_FALSE(recorded.overlapped.load());
}

TEST_CASE(
    "SynchronizerSink shares downstream media buffers and owns callbacks on "
    "one thread") {
  Recorded first;
  Recorded second;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, first);
  AddRecorder(sink, second);
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  const bool played = second.WaitVideos(4);
  sink.Stop();
  REQUIRE(played);
  REQUIRE(first.video.size() == second.video.size());
  for (std::size_t index = 0; index < first.video.size(); ++index) {
    CHECK(first.video[index].frame->data[0] ==
          second.video[index].frame->data[0]);
    CHECK(first.video[index].frame->pts == second.video[index].frame->pts);
  }
  CHECK(first.callback_thread == second.callback_thread);
  CHECK(first.callback_thread != std::this_thread::get_id());
}

TEST_CASE(
    "SynchronizerSink audio-only output keeps a continuous silence clock") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, true, false));
  sink.OnAudioFrame({1, Audio(96000)});
  const bool played = recorded.Wait([&] { return recorded.audio.size() >= 6; });
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool ended = recorded.Wait([&] { return !recorded.ends.empty(); });
  sink.Stop();
  REQUIRE(played);
  REQUIRE(ended);
  CHECK(recorded.video.empty());
  CheckClocks(recorded);
}

TEST_CASE("SynchronizerSink validates configuration and source contracts") {
  SECTION("zero queue") {
    auto config = Config();
    config.frame_queue_capacity = 0;
    CHECK_THROWS_AS(SynchronizerSink("synchronizer", config),
                    std::invalid_argument);
  }
  SECTION("上游视频帧率无效") {
    Recorded recorded;
    SynchronizerSink sink("synchronizer", Config());
    AddRecorder(sink, recorded);
    sink.OnStreamsReady(Streams(1, false, true, {0, 1}));
    const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
    const auto error = sink.error();
    sink.Stop();
    REQUIRE(failed);
    CHECK(error == "实时同步视频轨道缺少有效帧率");
  }
  SECTION("negative lateness") {
    auto config = Config();
    config.max_frame_lateness = -1ms;
    CHECK_THROWS_AS(SynchronizerSink("synchronizer", config),
                    std::invalid_argument);
  }
  SECTION("no consumer") {
    SynchronizerSink sink("synchronizer", Config());
    CHECK_THROWS_AS(sink.AddSink(nullptr), std::invalid_argument);
    CHECK_THROWS(sink.OnStreamsReady(Streams()));
  }
  SECTION("invalid frame") {
    Recorded recorded;
    SynchronizerSink sink("synchronizer", Config());
    AddRecorder(sink, recorded);
    sink.OnStreamsReady(Streams());
    CHECK_THROWS(sink.OnAudioFrame({1, Audio(AV_NOPTS_VALUE)}));
  }
  SECTION("undeclared track") {
    Recorded recorded;
    SynchronizerSink sink("synchronizer", Config());
    AddRecorder(sink, recorded);
    sink.OnStreamsReady(Streams(1, false, true));
    CHECK_THROWS(sink.OnAudioFrame({1, Audio(0)}));
  }
  SECTION("late registration") {
    Recorded recorded;
    SynchronizerSink sink("synchronizer", Config());
    AddRecorder(sink, recorded);
    sink.OnStreamsReady(Streams());
    CHECK_THROWS(sink.AddSink(std::make_unique<Recorder>(recorded)));
  }
}

TEST_CASE(
    "SynchronizerSink exposes worker failures and reports explicit fatal "
    "only") {
  bool fatal = false;
  SECTION("ordinary failure") {}
  SECTION("fatal failure") { fatal = true; }
  Recorded recorded;
  recorded.throw_video = !fatal;
  recorded.fatal_video = fatal;
  std::atomic<int> reports{0};
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.SetOnFatalError([&](const std::string&) { reports.fetch_add(1); });
  sink.OnStreamsReady(Streams(1, false, true));
  CHECK_NOTHROW(sink.OnVideoFrame({1, Video(0)}));
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(reports.load() == (fatal ? 1 : 0));
  CHECK(recorded.stops == 1);
}

TEST_CASE(
    "SynchronizerSink stops startup buffering without manufacturing EOF") {
  Recorded recorded;
  auto config = Config();
  config.frame_queue_capacity = 2;
  SynchronizerSink sink("synchronizer", config);
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams());
  for (int index = 0; index < 50; ++index)
    sink.OnAudioFrame({1, Audio(index * 960)});
  sink.Stop();
  sink.Stop();
  CHECK(recorded.audio.empty());
  CHECK(recorded.video.empty());
  CHECK(recorded.ends.empty());
  CHECK(recorded.stops == 1);
  CHECK(sink.queue_depth() == 0);
}

TEST_CASE(
    "SynchronizerSink connects media without implicitly routing messages") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  auto& child = AddRecorder(sink, recorded);
  int fatals = 0;
  int messages = 0;
  sink.SetMessageSender([&](const SinkMessage&) { ++messages; });
  sink.SetOnFatalError([&](const std::string& error) {
    ++fatals;
    CHECK(error == "child fatal");
  });
  sink.OnStreamsReady(Streams());
  REQUIRE(recorded.Wait([&] { return !recorded.streams.empty(); }));
  child.Message({"child", "unbound", nullptr, 0, std::nullopt});
  CHECK(messages == 0);
  child.Fatal("child fatal");
  child.Fatal("duplicate");
  sink.Stop();
  CHECK(fatals == 1);
  CHECK(recorded.stops == 1);
}

TEST_CASE("SynchronizerSink child uses its explicitly injected sender") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  auto& child = AddRecorder(sink, recorded);
  std::string message_type;
  child.SetMessageSender(
      [&](const SinkMessage& message) { message_type = message.type; });
  sink.OnStreamsReady(Streams());
  REQUIRE(recorded.Wait([&] { return !recorded.streams.empty(); }));
  child.Message({"child", "control", nullptr, 0, std::nullopt});
  CHECK(message_type == "control");
  sink.Stop();
}

TEST_CASE(
    "SynchronizerSink preserves child fatal and injected sender during Stop") {
  std::promise<std::string> received;
  auto message = received.get_future();
  SynchronizerSink sink("synchronizer", Config());
  auto reporter = std::make_unique<StopReporter>();
  auto ready = reporter->ready.get_future();
  reporter->SetMessageSender([&](const SinkMessage& message) {
    received.set_value(std::string(message.type));
  });
  sink.AddSink(std::move(reporter));
  std::atomic<int> fatals{0};
  sink.SetOnFatalError([&](const std::string&) { fatals.fetch_add(1); });
  sink.OnStreamsReady(Streams());
  REQUIRE(ready.wait_for(3s) == std::future_status::ready);
  sink.Stop();
  REQUIRE(message.wait_for(3s) == std::future_status::ready);
  CHECK(message.get() == "stopping");
  CHECK(fatals.load() == 1);
}

TEST_CASE("SynchronizerSink rejects a changed video prototype on its worker") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  const bool initial = recorded.WaitVideos(1);
  auto changed = Video(50);
  changed->width = 32;
  sink.OnVideoFrame({1, changed});
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  REQUIRE(initial);
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recorded.stops == 1);
}

TEST_CASE(
    "SynchronizerSink EOF without all prototypes fails instead of synthesizing "
    "unknown formats") {
  Recorded recorded;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams());
  sink.OnAudioFrame({1, Audio(0)});
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recorded.audio.empty());
  CHECK(recorded.video.empty());
}

TEST_CASE(
    "SynchronizerSink reports fatal from failed-end cleanup after an ordinary "
    "frame failure") {
  Recorded recorded;
  recorded.throw_video = true;
  recorded.fatal_end = true;
  std::atomic<int> fatal_reports{0};
  std::string fatal_message;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, recorded);
  sink.SetOnFatalError([&](const std::string& error) {
    fatal_message = error;
    fatal_reports.fetch_add(1);
  });
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK(sink.error() == "synchronizer output failed");
  REQUIRE(recorded.ends.size() == 1);
  CHECK(recorded.ends.front().reason == StreamEndReason::kFailed);
  CHECK(fatal_reports.load() == 1);
  CHECK(fatal_message == "synchronizer end fatal");
  CHECK(recorded.stops == 1);
}

TEST_CASE(
    "SynchronizerSink delivers end to remaining consumers after an earlier end "
    "callback throws") {
  Recorded first;
  Recorded second;
  first.throw_end = true;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, first);
  AddRecorder(sink, second);
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool second_ended = second.Wait([&] { return !second.ends.empty(); });
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  REQUIRE(second_ended);
  REQUIRE(failed);
  REQUIRE(first.ends.size() == 1);
  REQUIRE(second.ends.size() == 1);
  CHECK(first.ends.front().reason == StreamEndReason::kEof);
  CHECK(second.ends.front().reason == StreamEndReason::kEof);
  CHECK(first.stops == 1);
  CHECK(second.stops == 1);
  CHECK(sink.error() == "synchronizer end failed");
}

TEST_CASE(
    "SynchronizerSink only ends consumers whose ready callback was invoked "
    "during partial startup") {
  Recorded first;
  Recorded second;
  Recorded third;
  second.throw_ready = true;
  SynchronizerSink sink("synchronizer", Config());
  AddRecorder(sink, first);
  AddRecorder(sink, second);
  AddRecorder(sink, third);
  sink.OnStreamsReady(Streams(1, false, true));
  const bool failed = WaitState(sink, SynchronizerSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK(first.streams.size() == 1);
  CHECK(second.streams.size() == 1);
  CHECK(third.streams.empty());
  REQUIRE(first.ends.size() == 1);
  REQUIRE(second.ends.size() == 1);
  CHECK(first.ends.front().reason == StreamEndReason::kFailed);
  CHECK(second.ends.front().reason == StreamEndReason::kFailed);
  CHECK(third.ends.empty());
  CHECK(first.stops == 1);
  CHECK(second.stops == 1);
  CHECK(third.stops == 1);
}

TEST_CASE(
    "SynchronizerSink rejects reset after final EOF admission even before the "
    "worker can drain") {
  Recorded recorded;
  recorded.block_video = true;
  SynchronizerSink sink("synchronizer", Config());
  ReleaseOnExit release{recorded};
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true));
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recorded.Wait([&] { return recorded.video_entered; });
  sink.OnVideoFrame({1, Video(50)});
  sink.OnInputEnded({1, StreamEndReason::kEof});
  bool rejected = false;
  try {
    sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  } catch (const std::logic_error&) {
    rejected = true;
  }
  bool end_pending;
  {
    std::lock_guard<std::mutex> lock(recorded.mutex);
    end_pending = recorded.ends.empty();
  }
  recorded.Release();
  sink.Stop();
  REQUIRE(entered);
  CHECK(end_pending);
  CHECK(rejected);
  CHECK(recorded.resets.empty());
  CHECK(recorded.streams.size() == 1);
  CHECK(recorded.stops == 1);
}

TEST_CASE("SynchronizerSink stop interrupts a distant playback deadline") {
  Recorded recorded;
  auto config = Config();
  SynchronizerSink sink("synchronizer", config);
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true, {1, 1}));
  sink.OnVideoFrame({1, Video(0)});
  const bool played = recorded.WaitVideos(1);
  const auto start = Clock::now();
  sink.Stop();
  const auto elapsed = Clock::now() - start;
  REQUIRE(played);
  CHECK(elapsed < 500ms);
  CHECK(recorded.stops == 1);
  CHECK(recorded.ends.empty());
}

TEST_CASE(
    "SynchronizerSink processes queued EOF when every video callback exceeds "
    "its frame interval") {
  Recorded recorded;
  recorded.video_delay = 60ms;
  auto config = Config();
  SynchronizerSink sink("synchronizer", config);
  AddRecorder(sink, recorded);
  sink.OnStreamsReady(Streams(1, false, true, {25, 1}));
  sink.OnVideoFrame({1, Video(0)});
  const bool played = recorded.WaitVideos(1);
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const auto deadline = Clock::now() + 1s;
  while (sink.state() != SynchronizerSinkState::kEnded &&
         sink.state() != SynchronizerSinkState::kFailed &&
         Clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool ended = sink.state() == SynchronizerSinkState::kEnded;
  sink.Stop();
  INFO(sink.error());
  REQUIRE(played);
  REQUIRE(ended);
  REQUIRE(recorded.ends.size() == 1);
  CHECK(recorded.ends.front().reason == StreamEndReason::kEof);
  CHECK(recorded.stops == 1);
}

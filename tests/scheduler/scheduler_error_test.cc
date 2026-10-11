#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <string>
#include <string_view>

#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

TEST_CASE("Scheduler 配置错误只返回启动失败", "[scheduler][error]") {
  SchedulerConfig config;
  config.audio_sample_rate = 0;
  std::atomic<int> errors{0};
  Scheduler scheduler(config);
  scheduler.SetOnError(
      [&](int, std::string_view) noexcept { errors.fetch_add(1); });
  CHECK_FALSE(scheduler.Start(Streams(true, true)));
  scheduler.Drain();
  scheduler.Stop();
  CHECK(errors.load() == 0);
}

TEST_CASE("Scheduler 重采样致命错误终止会话且 Stop 后可重启",
          "[scheduler][error]") {
  int errors = 0;
  int code = 0;
  std::string message;
  Rig rig;
  rig.scheduler.SetOnError([&](int error, std::string_view reason) noexcept {
    rig.errors.Run([&] {
      std::lock_guard<std::mutex> lock(rig.mutex);
      ++errors;
      code = error;
      message.assign(reason);
      rig.changed.notify_all();
    });
  });
  auto invalid = Audio(128, 0, 44100, AV_SAMPLE_FMT_S16);
  // FFmpeg rejects the zero rate while initializing SWR, before reading PCM.
  invalid->sample_rate = 0;
  for (int session = 1; session <= 2; ++session) {
    REQUIRE(rig.Initialize(true, true));
    CHECK_FALSE(rig.scheduler.SubmitAudio(invalid));
    REQUIRE(rig.Wait([&] { return errors == session; }));
    CHECK_FALSE(rig.scheduler.SubmitAudio(invalid));
    CHECK_FALSE(rig.scheduler.SubmitAudio(Audio(1024)));
    CHECK_FALSE(rig.scheduler.SubmitVideo(Video(1, 0)));
    rig.scheduler.Drain();
    rig.Finish();
    CHECK(errors == session);
    CHECK(code == AVERROR(EINVAL));
    CHECK(message.find("初始化音频重采样器") != std::string::npos);
    CHECK(rig.ended == 0);
    CHECK(rig.stopped == session);
    CHECK(rig.videos.empty());
    CHECK(rig.audios.empty());
  }
  REQUIRE(rig.Initialize(false, true));
  REQUIRE(rig.scheduler.SubmitAudio(Audio(1024)));
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(errors == 2);
  CHECK(rig.stopped == 3);
  CHECK_FALSE(rig.audios.empty());
}

TEST_CASE("Scheduler 视频缓存满丢弃后继续接受并排空", "[scheduler][error]") {
  Gate gate;
  std::atomic<int> errors{0};
  int last_video = 0;
  Rig rig;
  GateRelease release(gate);
  rig.scheduler.SetOnError(
      [&](int, std::string_view) noexcept { errors.fetch_add(1); });
  rig.SetVideoSink([&](const ffmpeg::Frame& frame) {
    {
      std::lock_guard<std::mutex> lock(rig.mutex);
      last_video = frame->data[0][0];
      rig.changed.notify_all();
    }
    gate.Block();
  });
  REQUIRE(rig.Initialize(true, false));
  REQUIRE(rig.scheduler.SubmitVideo(Video(1, 0)));
  REQUIRE(gate.WaitEntered());
  for (int i = 0; i < 30; ++i) {
    REQUIRE(rig.scheduler.SubmitVideo(Video(2, (i + 1) * 33'333'333LL)));
  }
  CHECK_FALSE(rig.scheduler.SubmitVideo(Video(3, 2'000'000'000)));
  REQUIRE(rig.scheduler.SubmitVideo(Video(99, 3'000'000'000)));
  gate.Release();
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(last_video == 99);
  CHECK(errors.load() == 0);
}

TEST_CASE("Scheduler 的 Stop 等待输入线程上的 OnError 回调返回",
          "[scheduler][error][lifecycle]") {
  Gate gate;
  std::mutex mutex;
  std::condition_variable changed;
  bool stop_started = false;
  bool stop_done = false;
  bool accepted = true;
  std::atomic<int> errors{0};
  std::atomic<int> ended{0};
  Scheduler scheduler;
  GateRelease release(gate);
  scheduler.SetOnError([&](int, std::string_view) noexcept {
    errors.fetch_add(1);
    gate.Block();
  });
  scheduler.SetOnEnded([&]() noexcept { ended.fetch_add(1); });
  REQUIRE(scheduler.Start(Streams(true, true)));
  auto invalid = Audio(128, 0, 44100, AV_SAMPLE_FMT_S16);
  invalid->sample_rate = 0;
  std::thread input([&] { accepted = scheduler.SubmitAudio(invalid); });
  const bool entered = gate.WaitEntered();
  std::thread stop([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop_started = true;
      changed.notify_all();
    }
    scheduler.Stop();
    std::lock_guard<std::mutex> lock(mutex);
    stop_done = true;
    changed.notify_all();
  });
  bool stopping = false;
  bool returned_early = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    stopping = changed.wait_for(lock, 2s, [&] { return stop_started; });
    returned_early = changed.wait_for(lock, 60ms, [&] { return stop_done; });
  }
  gate.Release();
  input.join();
  stop.join();
  CHECK(entered);
  CHECK(stopping);
  CHECK_FALSE(returned_early);
  CHECK(stop_done);
  CHECK_FALSE(accepted);
  CHECK(errors.load() == 1);
  CHECK(ended.load() == 0);
  CHECK_FALSE(scheduler.SubmitVideo(Video(1, 0)));
}

TEST_CASE("Scheduler 音频缓存满丢弃后继续接受并排空", "[scheduler][error]") {
  std::atomic<int> errors{0};
  Rig rig;
  rig.scheduler.SetOnError(
      [&](int, std::string_view) noexcept { errors.fetch_add(1); });
  REQUIRE(rig.Initialize(false, true));
  CHECK_FALSE(rig.scheduler.SubmitAudio(Audio(1000 * 1024 + 1)));
  REQUIRE(rig.scheduler.SubmitAudio(Audio(1024)));
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(ended);
  CHECK(errors.load() == 0);
  CHECK_FALSE(rig.audios.empty());
}

}  // namespace

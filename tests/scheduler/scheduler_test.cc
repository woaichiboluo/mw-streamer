#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

TEST_CASE("Scheduler空输入Drain只结束一次并可重新初始化",
          "[scheduler][lifecycle]") {
  Rig rig;
  REQUIRE(rig.Initialize(true, true));
  rig.scheduler.Drain();
  rig.scheduler.Drain();
  const bool first = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  rig.Stop();
  CHECK(first);
  CHECK(rig.stopped == 1);
  CHECK(rig.stop_thread == std::this_thread::get_id());
  CHECK_FALSE(rig.scheduler.SubmitAudio(Audio(16)));
  REQUIRE(rig.Initialize(true, false));
  REQUIRE(rig.scheduler.SubmitVideo(Video(9, 7'000'000'000)));
  rig.scheduler.Drain();
  const bool second = rig.Wait([&] { return rig.ended == 2; });
  rig.Finish();
  CHECK(second);
  CHECK(rig.stopped == 2);
  REQUIRE_FALSE(rig.videos.empty());
  CHECK(rig.videos.back().id == 9);
}

TEST_CASE("Scheduler在Processor拒绝Ready后不启动输出",
          "[scheduler][lifecycle]") {
  Rig rig;
  rig.processor.SetOnReady([](const auto&, const auto&) { return false; });
  CHECK_FALSE(rig.Initialize(true, true));
  CHECK_FALSE(rig.scheduler.SubmitVideo(Video(1, 0)));
  CHECK_FALSE(rig.scheduler.SubmitAudio(Audio(1024)));
  rig.scheduler.Drain();
  rig.Finish();
  CHECK(rig.ended == 0);
  CHECK(rig.stopped == 0);
  CHECK(rig.videos.empty());
  CHECK(rig.audios.empty());
}

TEST_CASE("Scheduler的音视频滤镜允许并发且Stop等待音频返回",
          "[scheduler][lifecycle]") {
  Gate gate;
  bool video_during_audio = false;
  bool stop_started = false;
  bool stop_done = false;
  Rig rig;
  GateRelease release(gate);
  rig.processor.SetOnAudio([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      gate.Block();
      return frame.Ref();
    });
  });
  rig.processor.SetOnVideo([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      std::lock_guard<std::mutex> lock(rig.mutex);
      video_during_audio = true;
      rig.changed.notify_all();
      return frame.Ref();
    });
  });
  REQUIRE(rig.Initialize(true, true));
  auto audio = Audio(1024);
  const bool audio_submitted = rig.scheduler.SubmitAudio(audio);
  const bool entered = gate.WaitEntered();
  const bool queued = rig.scheduler.SubmitVideo(Video(1, 0));
  const bool concurrent = rig.Wait([&] { return video_during_audio; });
  std::thread stop([&] {
    {
      std::lock_guard<std::mutex> lock(rig.mutex);
      stop_started = true;
      rig.changed.notify_all();
    }
    rig.Stop();
    std::lock_guard<std::mutex> lock(rig.mutex);
    stop_done = true;
    rig.changed.notify_all();
  });
  const bool stopping = rig.Wait([&] { return stop_started; });
  const bool returned_early = rig.Wait([&] { return stop_done; }, 60ms);
  gate.Release();
  stop.join();
  rig.errors.Rethrow();
  CHECK(entered);
  CHECK(queued);
  CHECK(concurrent);
  CHECK(stopping);
  CHECK_FALSE(returned_early);
  CHECK(stop_done);
  CHECK(rig.stopped == 1);
  CHECK(audio_submitted);
  CHECK_FALSE(rig.scheduler.SubmitVideo(Video(2, 0)));
}

TEST_CASE("Scheduler的Stop等待输出回调并取消未排空EOS",
          "[scheduler][lifecycle]") {
  Gate gate;
  bool stop_done = false;
  Rig rig;
  GateRelease release(gate);
  rig.SetVideoSink(
      [&](const ffmpeg::Frame&) { rig.errors.Run([&] { gate.Block(); }); });
  REQUIRE(rig.Initialize(true, false));
  REQUIRE(rig.scheduler.SubmitVideo(Video(1, 0, 1'000'000'000)));
  const bool entered = gate.WaitEntered();
  rig.scheduler.Drain();
  std::thread stop([&] {
    rig.Stop();
    std::lock_guard<std::mutex> lock(rig.mutex);
    stop_done = true;
    rig.changed.notify_all();
  });
  const bool returned_early = rig.Wait([&] { return stop_done; }, 60ms);
  gate.Release();
  stop.join();
  rig.errors.Rethrow();
  CHECK(entered);
  CHECK_FALSE(returned_early);
  CHECK(stop_done);
  CHECK(rig.ended == 0);
  CHECK(rig.stopped == 1);
}

TEST_CASE("Scheduler的Stop等待调用线程上的OnEnded返回",
          "[scheduler][lifecycle]") {
  Gate gate;
  bool stop_done = false;
  Rig rig;
  GateRelease release(gate);
  rig.processor.SetOnEnded([&] { rig.errors.Run([&] { gate.Block(); }); });
  REQUIRE(rig.Initialize(false, false));
  std::thread drain([&] { rig.scheduler.Drain(); });
  const bool entered = gate.WaitEntered();
  std::thread stop([&] {
    rig.Stop();
    std::lock_guard<std::mutex> lock(rig.mutex);
    stop_done = true;
    rig.changed.notify_all();
  });
  const bool returned_early = rig.Wait([&] { return stop_done; }, 60ms);
  gate.Release();
  drain.join();
  stop.join();
  rig.errors.Rethrow();
  CHECK(entered);
  CHECK_FALSE(returned_early);
  CHECK(stop_done);
  CHECK(rig.stopped == 1);
}

TEST_CASE("Scheduler停止能取消音视频线程的长时间等待",
          "[scheduler][lifecycle]") {
  const bool video = GENERATE(true, false);
  SchedulerConfig config;
  config.video_frame_rate = {1, 10};
  config.audio_block_samples = 480000;
  Scheduler scheduler(config);
  REQUIRE(scheduler.Start(Streams(video, !video)));
  std::this_thread::sleep_for(10ms);
  const auto started = Clock::now();
  scheduler.Stop();
  CHECK(Clock::now() - started < 500ms);
}

}  // namespace

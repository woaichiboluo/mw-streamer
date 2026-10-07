#include <catch2/catch_test_macros.hpp>

#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

class VideoGapRig {
 public:
  VideoGapRig() {
    scheduler.SetOnVideo([this](const ffmpeg::Frame& frame) noexcept {
      std::unique_lock<std::mutex> lock(mutex);
      try {
        const auto id = frame->data[0][0];
        if (selected.empty() || selected.back() != id) selected.push_back(id);
      } catch (...) {
        error = std::current_exception();
      }
      if (watch_next_output) {
        next_output_id = frame->data[0][0];
        next_output_seen = true;
        watch_next_output = false;
        changed.notify_all();
      }
      if (!first_entered) {
        first_entered = true;
        first_output_pts = frame->pts;
        changed.notify_all();
        changed.wait(lock, [this] { return first_released; });
      }
      if (frame->data[0][0] == 1 && !empty_entered &&
          frame->pts - first_output_pts >= 100'000'000) {
        empty_entered = true;
        empty_output_pts = frame->pts;
        changed.notify_all();
        changed.wait(lock, [this] { return empty_released; });
      }
    });
    scheduler.SetOnEnded([this]() noexcept {
      std::lock_guard<std::mutex> lock(mutex);
      ended = true;
      changed.notify_all();
    });
  }

  ~VideoGapRig() {
    Release();
    scheduler.Stop();
  }

  template <typename Predicate>
  bool Wait(Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(3), predicate);
  }

  void ReleaseFirst() {
    std::lock_guard<std::mutex> lock(mutex);
    first_released = true;
    changed.notify_all();
  }

  // Arm while the first callback is gated. The callback already in progress
  // cannot satisfy this observation; it records the next output tick's frame.
  void ObserveNextOutput() {
    std::lock_guard<std::mutex> lock(mutex);
    watch_next_output = true;
    next_output_seen = false;
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    first_released = empty_released = true;
    changed.notify_all();
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::exception_ptr error;
  std::vector<int> selected;
  bool first_entered = false;
  bool first_released = false;
  bool empty_entered = false;
  bool empty_released = false;
  bool ended = false;
  bool watch_next_output = false;
  bool next_output_seen = false;
  int next_output_id = -1;
  std::int64_t first_output_pts = 0;
  std::int64_t empty_output_pts = 0;
  Scheduler scheduler;
};

TEST_CASE("Scheduler选帧后处理视频且重复输出复用原缓冲", "[scheduler][video]") {
  int calls = 0;
  bool correct_thread = true;
  bool second_selected = false;
  const auto caller = std::this_thread::get_id();
  Rig rig;
  rig.processor.SetOnVideo([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      std::lock_guard<std::mutex> lock(rig.mutex);
      ++calls;
      second_selected |= frame->data[0][0] == 2;
      correct_thread &= std::this_thread::get_id() != caller;
      rig.changed.notify_all();
      return frame.Ref();
    });
  });
  REQUIRE(rig.Initialize(true, false));
  auto first = Video(1, 5'000'000'000, 400'000'000);
  const auto* buffer = first->buf[0]->data;
  REQUIRE(rig.scheduler.SubmitVideo(first));
  first.Unref();
  const bool started = rig.Wait([&] { return rig.videos.size() >= 2; });
  const auto submitted = Clock::now();
  REQUIRE(rig.scheduler.SubmitVideo(Video(2, 5'400'000'000)));
  const bool premature = rig.Wait([&] { return second_selected; }, 100ms);
  const bool advanced = rig.Wait([&] { return second_selected; });
  rig.scheduler.Drain();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(started);
  CHECK_FALSE(premature);
  CHECK(advanced);
  CHECK(ended);
  CHECK(correct_thread);
  CHECK(calls == static_cast<int>(rig.videos.size()));
  REQUIRE(rig.videos.size() >= 3);
  CHECK(rig.videos[0].buffer == buffer);
  CHECK(rig.videos[1].buffer == buffer);
  bool found_second = false;
  for (const auto& item : rig.videos) {
    if (item.id == 2) {
      found_second = true;
      CHECK(item.time >= submitted + 100ms);
    }
  }
  CHECK(found_second);
}

TEST_CASE("Scheduler丢弃迟到视频后仅处理最近帧", "[scheduler][video]") {
  Gate gate;
  int outputs = 0;
  std::vector<int> filtered;
  Rig rig;
  GateRelease release(gate);
  rig.processor.SetOnVideo([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      std::lock_guard<std::mutex> lock(rig.mutex);
      const auto id = frame->data[0][0];
      if (filtered.empty() || filtered.back() != id) filtered.push_back(id);
      return frame.Ref();
    });
  });
  rig.SetVideoSink([&](const ffmpeg::Frame&) {
    rig.errors.Run([&] {
      if (++outputs == 1) gate.Block();
    });
  });
  REQUIRE(rig.Initialize(true, false));
  REQUIRE(rig.scheduler.SubmitVideo(Video(1, 6'000'000'000)));
  const bool entered = gate.WaitEntered();
  bool queued = true;
  for (int i = 2; i <= 4; ++i) {
    queued &= rig.scheduler.SubmitVideo(
        Video(i, 6'000'000'000 + (i - 1) * 10'000'000));
  }
  // Hold the consumer beyond all queued deadlines to create known lateness.
  rig.Wait([] { return false; }, 90ms);
  rig.scheduler.Drain();
  gate.Release();
  const bool ended = rig.Wait([&] { return rig.ended == 1; });
  rig.Finish();
  CHECK(entered);
  CHECK(queued);
  CHECK(ended);
  REQUIRE(filtered.size() == 2);
  CHECK(filtered[0] == 1);
  CHECK(filtered[1] == 4);
}

TEST_CASE("视频空队列重复输出不把媒体时钟推进到墙钟", "[scheduler][video]") {
  VideoGapRig rig;
  REQUIRE(rig.scheduler.Start(VideoStream()));
  constexpr std::int64_t kFirstPts = 30'000'000'000;
  REQUIRE(rig.scheduler.SubmitVideo(IdentifiedVideo(1, kFirstPts)));
  const bool first = rig.Wait([&] { return rig.first_entered; });
  rig.ReleaseFirst();
  // The output callback, rather than a sleep, proves at least 100 ms of output
  // ticks passed while the source queue was empty. Its gate keeps enqueueing
  // the following three frames atomic with respect to the next video tick.
  const bool empty = rig.Wait([&] { return rig.empty_entered; });
  bool accepted = true;
  for (int id = 2; id <= 4; ++id) {
    accepted &= rig.scheduler.SubmitVideo(
        IdentifiedVideo(id, kFirstPts + (id - 1) * 20'000'000));
  }
  rig.scheduler.Drain();
  rig.Release();
  const bool ended = rig.Wait([&] { return rig.ended; });
  rig.scheduler.Stop();
  if (rig.error) std::rethrow_exception(rig.error);
  CHECK(first);
  CHECK(empty);
  CHECK(accepted);
  CHECK(ended);
  CHECK(rig.empty_output_pts - rig.first_output_pts >= 100'000'000);
  REQUIRE(rig.selected.size() >= 2);
  CHECK(rig.selected[0] == 1);
  CHECK(rig.selected[1] == 2);
}

TEST_CASE("首视频PTS零哨兵令下一tick直接接收未来头帧", "[scheduler][video]") {
  VideoGapRig rig;
  REQUIRE(rig.scheduler.Start(VideoStream()));
  REQUIRE(rig.scheduler.SubmitVideo(IdentifiedVideo(1, 0)));
  const bool first = rig.Wait([&] { return rig.first_entered; });
  // Keep the worker gated while the future head arrives. The zero PTS sentinel
  // takes this head directly; a separate initialized flag would repeat id=1.
  const bool queued =
      rig.scheduler.SubmitVideo(IdentifiedVideo(2, 1'000'000'000));
  rig.ObserveNextOutput();
  rig.scheduler.Drain();
  rig.Release();
  const bool observed = rig.Wait([&] { return rig.next_output_seen; });
  rig.scheduler.Stop();
  if (rig.error) std::rethrow_exception(rig.error);
  CHECK(first);
  CHECK(queued);
  CHECK(observed);
  CHECK(rig.next_output_id == 2);
  REQUIRE(rig.selected.size() >= 2);
  CHECK(rig.selected[0] == 1);
  CHECK(rig.selected[1] == 2);
}

TEST_CASE("正PTS回退补偿保留无符号回绕的选帧顺序", "[scheduler][video]") {
  VideoGapRig rig;
  REQUIRE(rig.scheduler.Start(VideoStream()));
  REQUIRE(rig.scheduler.SubmitVideo(IdentifiedVideo(1, 5'000'000)));
  const bool first = rig.Wait([&] { return rig.first_entered; });
  const bool queued_head =
      rig.scheduler.SubmitVideo(IdentifiedVideo(2, 25'000'000));
  const bool queued_backward =
      rig.scheduler.SubmitVideo(IdentifiedVideo(3, 10'000'000));
  // The next tick is at least 33.333 ms later, making the 25 ms head ready.
  // Adjacent rollback then computes 10 ms - (25 ms - 5 ms). Unsigned uint64
  // arithmetic wraps this -10 ms result, discards id=2 and selects id=3 in the
  // same tick.
  rig.ObserveNextOutput();
  rig.scheduler.Drain();
  rig.Release();
  const bool observed = rig.Wait([&] { return rig.next_output_seen; });
  rig.scheduler.Stop();
  if (rig.error) std::rethrow_exception(rig.error);
  CHECK(first);
  CHECK(queued_head);
  CHECK(queued_backward);
  CHECK(observed);
  CHECK(rig.next_output_id == 3);
  REQUIRE(rig.selected.size() >= 2);
  CHECK(rig.selected[0] == 1);
  CHECK(rig.selected[1] == 3);
}

}  // namespace

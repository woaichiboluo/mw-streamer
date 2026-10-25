#include "mw/streamer/platform/wait_signal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <thread>

#include "mw/streamer/platform/write_signal.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::internal::WaitSignal;
using mw::streamer::internal::WriteSignal;
using Clock = std::chrono::steady_clock;

TEST_CASE("WaitSignal retains notifications preceding a wait",
          "[platform][wait_signal]") {
  WaitSignal signal;
  const auto revision = signal.GetRevision();
  signal.Notify();
  CHECK(signal.GetRevision() != revision);
  const auto started = Clock::now();
  CHECK(signal.WaitUntil(revision, started + 500ms));
  CHECK(Clock::now() - started < 250ms);
}

TEST_CASE("WaitSignal waits until its deadline without a notification",
          "[platform][wait_signal]") {
  WaitSignal signal;
  const auto revision = signal.GetRevision();
  const auto started = Clock::now();
  CHECK_FALSE(signal.WaitUntil(revision, started + 40ms));
  CHECK(Clock::now() - started >= 35ms);
  CHECK(Clock::now() - started < 1s);
  CHECK(signal.GetRevision() == revision);
}

TEST_CASE("WaitSignal notification wakes a timed waiter before its deadline",
          "[platform][wait_signal]") {
  WaitSignal signal;
  const auto revision = signal.GetRevision();
  std::promise<bool> completion;
  auto completed = completion.get_future();
  Clock::time_point finished;
  std::thread waiter([&] {
    const bool changed = signal.WaitUntil(revision, Clock::now() + 1s);
    finished = Clock::now();
    completion.set_value(changed);
  });
  const auto blocked = completed.wait_for(40ms);
  const auto notified = Clock::now();
  signal.Notify();
  const auto resumed = completed.wait_for(250ms);
  waiter.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get());
  CHECK(finished - notified < 250ms);
}

TEST_CASE("WaitSignal notification wakes every waiter sharing a revision",
          "[platform][wait_signal]") {
  WaitSignal signal;
  const auto revision = signal.GetRevision();
  std::array<std::promise<bool>, 2> completion;
  std::array<std::future<bool>, 2> completed{completion[0].get_future(),
                                             completion[1].get_future()};
  std::array<std::thread, 2> waiters;
  for (std::size_t index = 0; index < waiters.size(); ++index) {
    waiters[index] = std::thread([&, index] {
      completion[index].set_value(
          signal.WaitUntil(revision, Clock::now() + 1s));
    });
  }
  const auto first_blocked = completed[0].wait_for(40ms);
  const auto second_blocked = completed[1].wait_for(40ms);
  signal.Notify();
  const auto first_resumed = completed[0].wait_for(250ms);
  const auto second_resumed = completed[1].wait_for(250ms);
  for (auto& waiter : waiters) waiter.join();
  CHECK(first_blocked == std::future_status::timeout);
  CHECK(second_blocked == std::future_status::timeout);
  CHECK(first_resumed == std::future_status::ready);
  CHECK(second_resumed == std::future_status::ready);
  CHECK(completed[0].get());
  CHECK(completed[1].get());
}

TEST_CASE("WriteSignal keeps per-track revisions while waking a shared waiter",
          "[platform][wait_signal][write_signal]") {
  auto wake = std::make_shared<WaitSignal>();
  WriteSignal video(wake);
  WriteSignal audio(wake);
  const auto video_revision = video.GetRevision();
  const auto audio_revision = audio.GetRevision();
  const auto first_wake = wake->GetRevision();
  video.Notify();
  CHECK(video.GetRevision() != video_revision);
  CHECK(audio.GetRevision() == audio_revision);
  CHECK(wake->WaitUntil(first_wake, Clock::now() + 500ms));
  const auto second_wake = wake->GetRevision();
  const auto notified_video_revision = video.GetRevision();
  audio.Notify();
  CHECK(audio.GetRevision() != audio_revision);
  CHECK(video.GetRevision() == notified_video_revision);
  CHECK(wake->WaitUntil(second_wake, Clock::now() + 500ms));
}

TEST_CASE("WriteSignal works without owning the shared wake signal",
          "[platform][wait_signal][write_signal]") {
  WriteSignal standalone;
  const auto standalone_revision = standalone.GetRevision();
  standalone.Notify();
  CHECK(standalone.GetRevision() != standalone_revision);
  auto wake = std::make_shared<WaitSignal>();
  std::weak_ptr<WaitSignal> weak = wake;
  WriteSignal signal(wake);
  wake.reset();
  REQUIRE(weak.expired());
  const auto revision = signal.GetRevision();
  signal.Notify();
  CHECK(signal.GetRevision() != revision);
}

TEST_CASE(
    "WriteSignal retains concurrent notifications independently per track",
    "[platform][wait_signal][write_signal]") {
  auto wake = std::make_shared<WaitSignal>();
  WriteSignal video(wake);
  WriteSignal audio(wake);
  const auto video_revision = video.GetRevision();
  const auto audio_revision = audio.GetRevision();
  const auto wake_revision = wake->GetRevision();
  std::array<std::thread, 4> producers;
  for (std::size_t index = 0; index < producers.size(); ++index) {
    producers[index] = std::thread([&, index] {
      auto& signal = index % 2 == 0 ? video : audio;
      for (int notification = 0; notification < 100; ++notification)
        signal.Notify();
    });
  }
  for (auto& producer : producers) producer.join();
  CHECK(video.GetRevision() - video_revision == 200);
  CHECK(audio.GetRevision() - audio_revision == 200);
  CHECK(wake->GetRevision() - wake_revision == 400);
}

}  // namespace

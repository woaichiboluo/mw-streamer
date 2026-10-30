#include "mw/streamer/platform/blocking_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/platform/wait_signal.h"
#include "mw/streamer/platform/write_signal.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::BlockingQueue;
using mw::streamer::internal::WaitSignal;
using mw::streamer::internal::WriteSignal;
using Result = mw::streamer::BlockingQueueResult;

TEST_CASE("BlockingQueue enforces positive capacity and FIFO order",
          "[platform][blocking_queue]") {
  CHECK_THROWS_AS(BlockingQueue<int>(0), std::invalid_argument);
  BlockingQueue<int> queue(3);
  CHECK(queue.Capacity() == 3);
  CHECK(queue.Size() == 0);
  const int first = 11;
  REQUIRE(queue.Push(first) == Result::kSuccess);
  REQUIRE(queue.Push(22) == Result::kSuccess);
  REQUIRE(queue.Push(33, 0ms) == Result::kSuccess);
  CHECK(queue.Size() == 3);
  int output = -1;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 11);
  REQUIRE(queue.Pop(output) == Result::kSuccess);
  CHECK(output == 22);
  REQUIRE(queue.Pop(output) == Result::kSuccess);
  CHECK(output == 33);
  CHECK(queue.TryPop(output) == Result::kEmpty);
  CHECK(output == 33);
  CHECK(queue.Size() == 0);
}

TEST_CASE("BlockingQueue failed operations preserve move-only ownership",
          "[platform][blocking_queue]") {
  BlockingQueue<std::unique_ptr<int>> queue(1);
  auto first = std::make_unique<int>(1);
  const auto* first_storage = first.get();
  REQUIRE(queue.Push(std::move(first)) == Result::kSuccess);
  CHECK_FALSE(first);
  auto rejected = std::make_unique<int>(2);
  const auto* rejected_storage = rejected.get();
  CHECK(queue.Push(std::move(rejected), 0ms) == Result::kTimeout);
  CHECK(rejected.get() == rejected_storage);
  queue.Close();
  queue.Close();
  CHECK(queue.Push(std::move(rejected)) == Result::kClosed);
  CHECK(rejected.get() == rejected_storage);
  CHECK(queue.Push(std::move(rejected), 1s) == Result::kClosed);
  CHECK(rejected.get() == rejected_storage);
  std::unique_ptr<int> output;
  REQUIRE(queue.Pop(output) == Result::kSuccess);
  CHECK(output.get() == first_storage);
  CHECK(queue.Pop(output) == Result::kClosed);
  CHECK(output.get() == first_storage);
  CHECK(queue.TryPop(output) == Result::kClosed);
  CHECK(output.get() == first_storage);
  BlockingQueue<std::unique_ptr<int>> empty(1);
  CHECK(empty.TryPop(output) == Result::kEmpty);
  CHECK(output.get() == first_storage);
}

TEST_CASE("BlockingQueue blocking Push wakes when a consumer frees capacity",
          "[platform][blocking_queue]") {
  BlockingQueue<int> queue(1);
  REQUIRE(queue.Push(1) == Result::kSuccess);
  std::promise<Result> completion;
  auto completed = completion.get_future();
  std::thread producer([&] { completion.set_value(queue.Push(2)); });
  const auto blocked = completed.wait_for(40ms);
  int first = 0;
  const auto consumed = queue.TryPop(first);
  const auto resumed = completed.wait_for(1s);
  queue.Close();
  producer.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(consumed == Result::kSuccess);
  CHECK(first == 1);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get() == Result::kSuccess);
  int second = 0;
  REQUIRE(queue.Pop(second) == Result::kSuccess);
  CHECK(second == 2);
}

TEST_CASE("BlockingQueue blocking Pop wakes when a producer inserts an item",
          "[platform][blocking_queue]") {
  BlockingQueue<int> queue(1);
  std::promise<Result> completion;
  auto completed = completion.get_future();
  int output = -1;
  std::thread consumer([&] { completion.set_value(queue.Pop(output)); });
  const auto blocked = completed.wait_for(40ms);
  const auto inserted = queue.Push(7, 0ms);
  const auto resumed = completed.wait_for(1s);
  queue.Close();
  consumer.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(inserted == Result::kSuccess);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get() == Result::kSuccess);
  CHECK(output == 7);
}

TEST_CASE(
    "BlockingQueue timed Push waits for its deadline without consuming input",
    "[platform][blocking_queue]") {
  BlockingQueue<std::unique_ptr<int>> queue(1);
  REQUIRE(queue.Push(std::make_unique<int>(1)) == Result::kSuccess);
  auto input = std::make_unique<int>(2);
  const auto* storage = input.get();
  const auto started = std::chrono::steady_clock::now();
  CHECK(queue.Push(std::move(input), 40ms) == Result::kTimeout);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed >= 35ms);
  CHECK(elapsed < 1s);
  CHECK(input.get() == storage);
  CHECK(queue.Size() == 1);
}

TEST_CASE("BlockingQueue timed Push wakes before its timeout when space opens",
          "[platform][blocking_queue]") {
  using Clock = std::chrono::steady_clock;
  BlockingQueue<int> queue(1);
  REQUIRE(queue.Push(1) == Result::kSuccess);
  std::promise<Result> completion;
  auto completed = completion.get_future();
  Clock::time_point finished;
  std::thread producer([&] {
    const auto result = queue.Push(2, 1s);
    finished = Clock::now();
    completion.set_value(result);
  });
  const auto blocked = completed.wait_for(40ms);
  int output = 0;
  const auto freed = Clock::now();
  const auto consumed = queue.TryPop(output);
  const auto resumed = completed.wait_for(250ms);
  queue.Close();
  producer.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(consumed == Result::kSuccess);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get() == Result::kSuccess);
  CHECK(finished - freed < 250ms);
}

TEST_CASE(
    "BlockingQueue Close wakes every blocked producer and permits draining",
    "[platform][blocking_queue]") {
  BlockingQueue<std::unique_ptr<int>> queue(1);
  REQUIRE(queue.Push(std::make_unique<int>(1)) == Result::kSuccess);
  std::array<std::unique_ptr<int>, 2> inputs{std::make_unique<int>(2),
                                             std::make_unique<int>(3)};
  std::array<std::promise<Result>, 2> completion;
  std::array<std::future<Result>, 2> completed{completion[0].get_future(),
                                               completion[1].get_future()};
  std::array<std::thread, 2> producers;
  for (std::size_t index = 0; index < producers.size(); ++index) {
    producers[index] = std::thread([&, index] {
      completion[index].set_value(queue.Push(std::move(inputs[index])));
    });
  }
  const auto first_blocked = completed[0].wait_for(40ms);
  const auto second_blocked = completed[1].wait_for(40ms);
  queue.Close();
  for (auto& producer : producers) producer.join();
  CHECK(first_blocked == std::future_status::timeout);
  CHECK(second_blocked == std::future_status::timeout);
  for (std::size_t index = 0; index < completed.size(); ++index) {
    CHECK(completed[index].get() == Result::kClosed);
    REQUIRE(inputs[index]);
    CHECK(*inputs[index] == static_cast<int>(index) + 2);
  }
  std::unique_ptr<int> output;
  REQUIRE(queue.Pop(output) == Result::kSuccess);
  CHECK(*output == 1);
  CHECK(queue.Pop(output) == Result::kClosed);
  CHECK(*output == 1);
}

TEST_CASE(
    "BlockingQueue Close wakes every empty consumer without changing output",
    "[platform][blocking_queue]") {
  BlockingQueue<int> queue(1);
  std::array<int, 2> outputs{77, 88};
  std::array<std::promise<Result>, 2> completion;
  std::array<std::future<Result>, 2> completed{completion[0].get_future(),
                                               completion[1].get_future()};
  std::array<std::thread, 2> consumers;
  for (std::size_t index = 0; index < consumers.size(); ++index) {
    consumers[index] = std::thread(
        [&, index] { completion[index].set_value(queue.Pop(outputs[index])); });
  }
  const auto first_blocked = completed[0].wait_for(40ms);
  const auto second_blocked = completed[1].wait_for(40ms);
  queue.Close();
  for (auto& consumer : consumers) consumer.join();
  CHECK(first_blocked == std::future_status::timeout);
  CHECK(second_blocked == std::future_status::timeout);
  CHECK(completed[0].get() == Result::kClosed);
  CHECK(completed[1].get() == Result::kClosed);
  CHECK(outputs[0] == 77);
  CHECK(outputs[1] == 88);
}

TEST_CASE(
    "BlockingQueue delivers each item once with multiple producers and "
    "consumers",
    "[platform][blocking_queue]") {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 3;
  constexpr int kPerProducer = 200;
  BlockingQueue<int> queue(7);
  std::atomic<bool> failed{false};
  std::mutex mutex;
  std::condition_variable changed;
  int finished = 0;
  std::array<std::vector<int>, kConsumers> consumed;
  std::array<std::thread, kConsumers> consumers;
  for (std::size_t index = 0; index < consumers.size(); ++index) {
    consumers[index] = std::thread([&, index] {
      int item;
      while (true) {
        const auto result = queue.Pop(item);
        if (result == Result::kClosed) break;
        if (result != Result::kSuccess) {
          failed.store(true);
          break;
        }
        consumed[index].push_back(item);
      }
    });
  }
  std::array<std::thread, kProducers> producers;
  for (int index = 0; index < kProducers; ++index) {
    producers[static_cast<std::size_t>(index)] = std::thread([&, index] {
      for (int item = 0; item < kPerProducer; ++item) {
        if (queue.Push(index * kPerProducer + item) != Result::kSuccess) {
          failed.store(true);
          break;
        }
      }
      {
        std::lock_guard lock(mutex);
        ++finished;
      }
      changed.notify_all();
    });
  }
  bool produced;
  {
    std::unique_lock lock(mutex);
    produced =
        changed.wait_for(lock, 2s, [&] { return finished == kProducers; });
  }
  queue.Close();
  for (auto& producer : producers) producer.join();
  for (auto& consumer : consumers) consumer.join();
  CHECK(produced);
  CHECK_FALSE(failed.load());
  std::vector<int> all;
  for (const auto& items : consumed)
    all.insert(all.end(), items.begin(), items.end());
  std::sort(all.begin(), all.end());
  REQUIRE(all.size() == kProducers * kPerProducer);
  for (std::size_t index = 0; index < all.size(); ++index) {
    CHECK(all[index] == static_cast<int>(index));
  }
  CHECK(queue.Size() == 0);
}

struct ThrowingCopy {
  int value = 0;
  bool throw_on_copy = false;
  ThrowingCopy() = default;
  ThrowingCopy(const ThrowingCopy& other) : value(other.value) {
    if (other.throw_on_copy) throw std::runtime_error("copy insertion failed");
  }
  ThrowingCopy& operator=(const ThrowingCopy&) = default;
  ThrowingCopy(ThrowingCopy&&) = default;
  ThrowingCopy& operator=(ThrowingCopy&&) = default;
};

TEST_CASE("BlockingQueue remains usable after a copying insertion throws",
          "[platform][blocking_queue]") {
  BlockingQueue<ThrowingCopy> queue(1);
  ThrowingCopy input;
  input.value = 42;
  input.throw_on_copy = true;
  CHECK_THROWS_AS(queue.Push(input), std::runtime_error);
  CHECK(queue.Size() == 0);
  CHECK(input.value == 42);
  input.throw_on_copy = false;
  REQUIRE(queue.Push(input, 0ms) == Result::kSuccess);
  ThrowingCopy output;
  REQUIRE(queue.Pop(output) == Result::kSuccess);
  CHECK(output.value == 42);
  CHECK(queue.Size() == 0);
}

TEST_CASE("BlockingQueue shares one read signal across independent queues",
          "[platform][blocking_queue][wait_signal]") {
  using Clock = std::chrono::steady_clock;
  const bool video = GENERATE(false, true);
  BlockingQueue<int> video_queue(1);
  BlockingQueue<int> audio_queue(1);
  auto signal = std::make_shared<WaitSignal>();
  video_queue.SetReadSignal(signal);
  audio_queue.SetReadSignal(signal);
  const auto revision = signal->GetRevision();
  std::promise<bool> completion;
  auto completed = completion.get_future();
  std::thread waiter([&] {
    completion.set_value(signal->WaitUntil(revision, Clock::now() + 1s));
  });
  const auto blocked = completed.wait_for(40ms);
  auto& ready_queue = video ? video_queue : audio_queue;
  auto& idle_queue = video ? audio_queue : video_queue;
  const auto inserted = ready_queue.Push(7, 0ms);
  const auto resumed = completed.wait_for(250ms);
  waiter.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(inserted == Result::kSuccess);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get());
  int output = 0;
  REQUIRE(ready_queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 7);
  CHECK(idle_queue.TryPop(output) == Result::kEmpty);
  ready_queue.Close();
  REQUIRE(idle_queue.Push(8, 0ms) == Result::kSuccess);
  REQUIRE(idle_queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 8);
}

TEST_CASE(
    "BlockingQueue retains a push between the empty check and signal wait",
    "[platform][blocking_queue][wait_signal]") {
  using Clock = std::chrono::steady_clock;
  BlockingQueue<int> queue(1);
  auto signal = std::make_shared<WaitSignal>();
  queue.SetReadSignal(signal);
  const auto revision = signal->GetRevision();
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kEmpty);
  REQUIRE(queue.Push(17, 0ms) == Result::kSuccess);
  const auto started = Clock::now();
  CHECK(signal->WaitUntil(revision, started + 500ms));
  CHECK(Clock::now() - started < 250ms);
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 17);
}

TEST_CASE("BlockingQueue Close wakes its read signal without requiring a frame",
          "[platform][blocking_queue][wait_signal]") {
  using Clock = std::chrono::steady_clock;
  BlockingQueue<int> queue(1);
  auto signal = std::make_shared<WaitSignal>();
  queue.SetReadSignal(signal);
  const auto revision = signal->GetRevision();
  std::promise<bool> completion;
  auto completed = completion.get_future();
  std::thread waiter([&] {
    completion.set_value(signal->WaitUntil(revision, Clock::now() + 1s));
  });
  const auto blocked = completed.wait_for(40ms);
  queue.Close();
  const auto resumed = completed.wait_for(250ms);
  waiter.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get());
  int output = 91;
  CHECK(queue.TryPop(output) == Result::kClosed);
  CHECK(output == 91);
}

TEST_CASE("BlockingQueue binding signals existing data or closure immediately",
          "[platform][blocking_queue][wait_signal]") {
  using Clock = std::chrono::steady_clock;
  const bool closed = GENERATE(false, true);
  BlockingQueue<int> queue(1);
  if (closed) {
    queue.Close();
  } else {
    REQUIRE(queue.Push(9) == Result::kSuccess);
  }
  auto signal = std::make_shared<WaitSignal>();
  const auto revision = signal->GetRevision();
  queue.SetReadSignal(signal);
  const auto started = Clock::now();
  CHECK(signal->WaitUntil(revision, started + 500ms));
  CHECK(Clock::now() - started < 250ms);
  int output = 0;
  CHECK(queue.TryPop(output) == (closed ? Result::kClosed : Result::kSuccess));
  if (!closed) CHECK(output == 9);
}

TEST_CASE("BlockingQueue replaces and detaches weak read signals",
          "[platform][blocking_queue][wait_signal]") {
  BlockingQueue<int> queue(2);
  auto first = std::make_shared<WaitSignal>();
  auto second = std::make_shared<WaitSignal>();
  queue.SetReadSignal(first);
  REQUIRE(queue.Push(1) == Result::kSuccess);
  const auto first_revision = first->GetRevision();
  const auto second_revision = second->GetRevision();
  queue.SetReadSignal(second);
  CHECK(second->GetRevision() != second_revision);
  const auto bound_revision = second->GetRevision();
  REQUIRE(queue.Push(2) == Result::kSuccess);
  CHECK(first->GetRevision() == first_revision);
  CHECK(second->GetRevision() != bound_revision);
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 1);
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 2);
  const auto detached_revision = second->GetRevision();
  queue.SetReadSignal({});
  REQUIRE(queue.Push(3) == Result::kSuccess);
  queue.Close();
  CHECK(first->GetRevision() == first_revision);
  CHECK(second->GetRevision() == detached_revision);
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 3);
  CHECK(queue.TryPop(output) == Result::kClosed);
}

TEST_CASE("BlockingQueue does not retain an expired read signal",
          "[platform][blocking_queue][wait_signal]") {
  BlockingQueue<int> queue(1);
  std::weak_ptr<WaitSignal> expired;
  {
    auto signal = std::make_shared<WaitSignal>();
    expired = signal;
    queue.SetReadSignal(signal);
  }
  REQUIRE(expired.expired());
  REQUIRE(queue.Push(31, 0ms) == Result::kSuccess);
  queue.Close();
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 31);
  CHECK(queue.TryPop(output) == Result::kClosed);
}

TEST_CASE("BlockingQueue read-signal cancellation leaves the source queue open",
          "[platform][blocking_queue][wait_signal]") {
  using Clock = std::chrono::steady_clock;
  BlockingQueue<int> queue(1);
  auto signal = std::make_shared<WaitSignal>();
  queue.SetReadSignal(signal);
  const auto revision = signal->GetRevision();
  signal->Notify();
  CHECK(signal->WaitUntil(revision, Clock::now() + 500ms));
  int output = 0;
  CHECK(queue.TryPop(output) == Result::kEmpty);
  REQUIRE(queue.Push(23, 0ms) == Result::kSuccess);
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 23);
}

TEST_CASE("BlockingQueue capacity or closure wakes a full queue write waiter",
          "[platform][blocking_queue][write_signal]") {
  using Clock = std::chrono::steady_clock;
  const bool close = GENERATE(false, true);
  BlockingQueue<int> queue(1);
  REQUIRE(queue.Push(7) == Result::kSuccess);
  auto wake = std::make_shared<WaitSignal>();
  auto signal = std::make_shared<WriteSignal>(wake);
  const auto write_revision = signal->GetRevision();
  const auto revision = wake->GetRevision();
  queue.SetWriteSignal(signal);
  CHECK(signal->GetRevision() == write_revision);
  std::promise<bool> completion;
  auto completed = completion.get_future();
  std::thread waiter([&] {
    completion.set_value(wake->WaitUntil(revision, Clock::now() + 1s));
  });
  const auto blocked = completed.wait_for(40ms);
  int output = 0;
  Result consumed = Result::kEmpty;
  if (close) {
    queue.Close();
  } else {
    consumed = queue.TryPop(output);
  }
  const auto resumed = completed.wait_for(250ms);
  waiter.join();
  CHECK(blocked == std::future_status::timeout);
  CHECK(resumed == std::future_status::ready);
  CHECK(completed.get());
  CHECK(signal->GetRevision() != write_revision);
  if (close) {
    REQUIRE(queue.TryPop(output) == Result::kSuccess);
    CHECK(output == 7);
    CHECK(queue.TryPop(output) == Result::kClosed);
  } else {
    CHECK(consumed == Result::kSuccess);
    CHECK(output == 7);
    REQUIRE(queue.Push(8, 0ms) == Result::kSuccess);
    REQUIRE(queue.TryPop(output) == Result::kSuccess);
    CHECK(output == 8);
  }
}

TEST_CASE("BlockingQueue write readiness isolates queues sharing a wake signal",
          "[platform][blocking_queue][write_signal]") {
  BlockingQueue<int> video(1);
  BlockingQueue<int> audio(1);
  REQUIRE(video.Push(1) == Result::kSuccess);
  REQUIRE(audio.Push(2) == Result::kSuccess);
  auto wake = std::make_shared<WaitSignal>();
  auto video_signal = std::make_shared<WriteSignal>(wake);
  auto audio_signal = std::make_shared<WriteSignal>(wake);
  video.SetWriteSignal(video_signal);
  audio.SetWriteSignal(audio_signal);
  const auto video_revision = video_signal->GetRevision();
  const auto audio_revision = audio_signal->GetRevision();
  const auto wake_revision = wake->GetRevision();
  int output = 0;
  REQUIRE(video.TryPop(output) == Result::kSuccess);
  CHECK(output == 1);
  CHECK(video_signal->GetRevision() != video_revision);
  CHECK(audio_signal->GetRevision() == audio_revision);
  CHECK(wake->GetRevision() != wake_revision);
  const auto notified_video_revision = video_signal->GetRevision();
  wake->Notify();
  CHECK(video_signal->GetRevision() == notified_video_revision);
  CHECK(audio_signal->GetRevision() == audio_revision);
  REQUIRE(audio.TryPop(output) == Result::kSuccess);
  CHECK(output == 2);
  CHECK(audio_signal->GetRevision() != audio_revision);
  CHECK(video_signal->GetRevision() == notified_video_revision);
}

TEST_CASE("BlockingQueue retains space freed before the shared signal wait",
          "[platform][blocking_queue][write_signal]") {
  using Clock = std::chrono::steady_clock;
  BlockingQueue<int> queue(1);
  REQUIRE(queue.Push(5) == Result::kSuccess);
  auto wake = std::make_shared<WaitSignal>();
  auto signal = std::make_shared<WriteSignal>(wake);
  queue.SetWriteSignal(signal);
  const auto revision = wake->GetRevision();
  const auto write_revision = signal->GetRevision();
  CHECK(queue.Push(6, 0ms) == Result::kTimeout);
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  const auto started = Clock::now();
  CHECK(wake->WaitUntil(revision, started + 500ms));
  CHECK(Clock::now() - started < 250ms);
  CHECK(signal->GetRevision() != write_revision);
  REQUIRE(queue.Push(6, 0ms) == Result::kSuccess);
}

TEST_CASE("BlockingQueue write binding signals available capacity or closure",
          "[platform][blocking_queue][write_signal]") {
  using Clock = std::chrono::steady_clock;
  const bool closed = GENERATE(false, true);
  BlockingQueue<int> queue(1);
  if (closed) {
    REQUIRE(queue.Push(3) == Result::kSuccess);
    queue.Close();
  }
  auto wake = std::make_shared<WaitSignal>();
  auto signal = std::make_shared<WriteSignal>(wake);
  const auto revision = wake->GetRevision();
  const auto write_revision = signal->GetRevision();
  queue.SetWriteSignal(signal);
  CHECK(signal->GetRevision() != write_revision);
  CHECK(wake->WaitUntil(revision, Clock::now() + 500ms));
  CHECK(queue.Push(4, 0ms) == (closed ? Result::kClosed : Result::kSuccess));
}

TEST_CASE("BlockingQueue replaces and detaches weak write signals",
          "[platform][blocking_queue][write_signal]") {
  BlockingQueue<int> queue(2);
  auto wake = std::make_shared<WaitSignal>();
  auto first = std::make_shared<WriteSignal>(wake);
  auto second = std::make_shared<WriteSignal>(wake);
  queue.SetWriteSignal(first);
  const auto first_revision = first->GetRevision();
  const auto second_revision = second->GetRevision();
  queue.SetWriteSignal(second);
  CHECK(second->GetRevision() != second_revision);
  const auto bound_revision = second->GetRevision();
  REQUIRE(queue.Push(1) == Result::kSuccess);
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 1);
  CHECK(first->GetRevision() == first_revision);
  CHECK(second->GetRevision() != bound_revision);
  const auto detached_revision = second->GetRevision();
  queue.SetWriteSignal({});
  REQUIRE(queue.Push(2) == Result::kSuccess);
  queue.Close();
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 2);
  CHECK(first->GetRevision() == first_revision);
  CHECK(second->GetRevision() == detached_revision);
}

TEST_CASE("BlockingQueue delivery does not retain an expired write signal",
          "[platform][blocking_queue][write_signal]") {
  BlockingQueue<int> queue(1);
  auto wake = std::make_shared<WaitSignal>();
  std::weak_ptr<WriteSignal> expired;
  {
    auto signal = std::make_shared<WriteSignal>(wake);
    expired = signal;
    queue.SetWriteSignal(signal);
  }
  REQUIRE(expired.expired());
  const auto revision = wake->GetRevision();
  REQUIRE(queue.Push(19) == Result::kSuccess);
  int output = 0;
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  CHECK(output == 19);
  queue.Close();
  CHECK(queue.TryPop(output) == Result::kClosed);
  CHECK(wake->GetRevision() == revision);
}

TEST_CASE("BlockingQueue failed operations do not signal writable capacity",
          "[platform][blocking_queue][write_signal]") {
  BlockingQueue<int> queue(1);
  auto wake = std::make_shared<WaitSignal>();
  auto signal = std::make_shared<WriteSignal>(wake);
  queue.SetWriteSignal(signal);
  auto revision = wake->GetRevision();
  auto write_revision = signal->GetRevision();
  int output = 0;
  CHECK(queue.TryPop(output) == Result::kEmpty);
  REQUIRE(queue.Push(1, 0ms) == Result::kSuccess);
  CHECK(queue.Push(2, 0ms) == Result::kTimeout);
  CHECK(wake->GetRevision() == revision);
  CHECK(signal->GetRevision() == write_revision);
  queue.Close();
  revision = wake->GetRevision();
  write_revision = signal->GetRevision();
  CHECK(queue.Push(3, 0ms) == Result::kClosed);
  CHECK(wake->GetRevision() == revision);
  CHECK(signal->GetRevision() == write_revision);
  REQUIRE(queue.TryPop(output) == Result::kSuccess);
  revision = wake->GetRevision();
  write_revision = signal->GetRevision();
  CHECK(queue.TryPop(output) == Result::kClosed);
  CHECK(wake->GetRevision() == revision);
  CHECK(signal->GetRevision() == write_revision);
}

}  // namespace

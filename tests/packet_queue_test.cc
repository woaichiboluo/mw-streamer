#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "Poller/EventPoller.h"
#include "mw/cache/packet_queue.h"
#include "mw/input/player_proxy.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::cache::PacketQueue;
using mw::streamer::cache::PacketQueueState;
using mw::streamer::cache::PacketStream;
using mw::streamer::input::PlayerProxy;
using mw::streamer::input::PlayerState;

struct AvPacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

using PacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

PacketPtr MakePacket(int stream_index, std::int64_t dts, AVRational time_base) {
  PacketPtr packet(av_packet_alloc());
  if (!packet || av_new_packet(packet.get(), 4) < 0) {
    throw std::runtime_error("测试AVPacket分配失败");
  }
  packet->data[0] = static_cast<std::uint8_t>(stream_index + 1);
  packet->dts = dts;
  packet->pts = dts;
  packet->stream_index = stream_index;
  packet->time_base = time_base;
  return packet;
}

std::vector<PacketStream> MillisecondStreams() {
  return {
      {0, AVMEDIA_TYPE_VIDEO, {1, 1000}},
      {1, AVMEDIA_TYPE_AUDIO, {1, 1000}},
  };
}

bool WaitFor(std::condition_variable& condition, std::mutex& mutex,
             const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = 5s) {
  std::unique_lock<std::mutex> lock(mutex);
  return condition.wait_for(lock, timeout, predicate);
}

void RunOnPollerAndWait(const std::shared_ptr<toolkit::EventPoller>& poller,
                        std::function<void()> task) {
  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
  poller->async(
      [&, task = std::move(task)]() mutable {
        task();
        {
          std::lock_guard<std::mutex> lock(mutex);
          completed = true;
        }
        condition.notify_all();
      },
      false);
  REQUIRE(WaitFor(condition, mutex, [&]() { return completed; }));
}

bool Feed(PacketQueue& queue, std::uint64_t generation, int stream_index,
          std::int64_t dts, AVRational time_base = {1, 1000}) {
  auto packet = MakePacket(stream_index, dts, time_base);
  return queue.Input(generation, packet.get());
}

void StopAndWait(const PacketQueue::Ptr& queue) {
  queue->Stop();
  RunOnPollerAndWait(queue->poller(), []() {});
}

std::string SamplePath() {
  return std::string(MW_PACKET_QUEUE_TEST_DATA_DIR) + "/packet_queue_8s.mp4";
}

}  // namespace

TEST_CASE("packet queue waits for both audio and video prebuffer") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t output_count = 0;
  std::atomic_bool valid_payload = true;
  std::atomic_bool callback_on_poller = true;

  queue->SetOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    if (generation != 1 || !packet || packet->size != 4 ||
        packet->data[0] != packet->stream_index + 1) {
      valid_payload = false;
    }
    callback_on_poller =
        callback_on_poller && queue->poller()->isCurrentThread();
    ++output_count;
    condition.notify_all();
  });

  bool accepted = true;
  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, MillisecondStreams());
    for (std::int64_t dts = 0; dts <= 1200; dts += 100) {
      accepted = accepted && Feed(*queue, 1, 0, dts);
    }
    for (std::int64_t dts = 0; dts <= 900; dts += 100) {
      accepted = accepted && Feed(*queue, 1, 1, dts);
    }
  });

  std::this_thread::sleep_for(150ms);
  CHECK(output_count == 0);
  CHECK(queue->state() == PacketQueueState::kFilling);

  RunOnPollerAndWait(queue->poller(), [&]() {
    accepted = accepted && Feed(*queue, 1, 1, 1000);
  });

  REQUIRE(WaitFor(condition, mutex, [&]() { return output_count.load() > 0; }));
  CHECK(accepted);
  CHECK(valid_payload);
  CHECK(callback_on_poller);
  CHECK(queue->generation() == 1);

  StopAndWait(queue);
}

TEST_CASE("packet queue honors representative cache durations") {
  const auto cache_duration = GENERATE(1s, 5s, 15s, 30s);
  const auto cache_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(cache_duration)
          .count();
  std::vector<PacketStream> streams;

  SECTION("audio only") { streams = {{3, AVMEDIA_TYPE_AUDIO, {1, 1000}}}; }
  SECTION("video only") { streams = {{7, AVMEDIA_TYPE_VIDEO, {1, 1000}}}; }
  SECTION("audio and video") { streams = MillisecondStreams(); }

  auto queue = std::make_shared<PacketQueue>(cache_duration);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t output_count = 0;

  queue->SetPlaybackRate(20.0);
  queue->SetOnPacket([&](std::uint64_t, const AVPacket*) {
    ++output_count;
    condition.notify_all();
  });

  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, streams);
    for (std::int64_t dts = 0; dts < cache_duration_ms; dts += 100) {
      for (const auto& stream : streams) {
        Feed(*queue, 1, stream.stream_index, dts);
      }
    }
  });

  CHECK(output_count == 0);
  CHECK(queue->state() == PacketQueueState::kFilling);

  RunOnPollerAndWait(queue->poller(), [&]() {
    for (const auto& stream : streams) {
      Feed(*queue, 1, stream.stream_index, cache_duration_ms);
    }
    queue->EndInput(1);
  });

  const auto expected_packets =
      static_cast<std::size_t>((cache_duration_ms / 100 + 1) * streams.size());
  REQUIRE(WaitFor(condition, mutex,
                  [&]() { return output_count.load() == expected_packets; }));
  CHECK(queue->state() == PacketQueueState::kStarved);

  StopAndWait(queue);
}

TEST_CASE("packet queue buffers and drains a single configured track") {
  AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN;
  int stream_index = -1;

  SECTION("audio only") {
    media_type = AVMEDIA_TYPE_AUDIO;
    stream_index = 3;
  }
  SECTION("video only") {
    media_type = AVMEDIA_TYPE_VIDEO;
    stream_index = 7;
  }

  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::int64_t> output_dts;
  std::atomic_bool output_matches_stream = true;

  queue->SetPlaybackRate(20.0);
  queue->SetOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    output_matches_stream = output_matches_stream && generation == 1 &&
                            packet->stream_index == stream_index;
    {
      std::lock_guard<std::mutex> lock(mutex);
      output_dts.push_back(packet->dts);
    }
    condition.notify_all();
  });

  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, {{stream_index, media_type, {1, 1000}}});
    for (std::int64_t dts = 0; dts <= 900; dts += 100) {
      Feed(*queue, 1, stream_index, dts);
    }
  });

  CHECK(output_dts.empty());
  CHECK(queue->state() == PacketQueueState::kFilling);

  RunOnPollerAndWait(queue->poller(), [&]() {
    for (std::int64_t dts = 1000; dts <= 1200; dts += 100) {
      Feed(*queue, 1, stream_index, dts);
    }
    queue->EndInput(1);
  });

  REQUIRE(WaitFor(condition, mutex, [&]() { return output_dts.size() == 13; }));
  RunOnPollerAndWait(queue->poller(), []() {});

  std::vector<std::int64_t> output_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    output_snapshot = output_dts;
  }
  CHECK(output_matches_stream);
  REQUIRE(output_snapshot.size() == 13);
  for (std::size_t index = 0; index < output_snapshot.size(); ++index) {
    CHECK(output_snapshot[index] == static_cast<std::int64_t>(index) * 100);
  }
  CHECK(queue->packet_count() == 0);
  CHECK(queue->state() == PacketQueueState::kStarved);

  StopAndWait(queue);
}

TEST_CASE("packet queue self drives cached packets and stops when empty") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t output_count = 0;
  std::chrono::steady_clock::time_point first_output;
  std::chrono::steady_clock::time_point last_output;

  queue->SetOnPacket([&](std::uint64_t, const AVPacket*) {
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (output_count == 0) {
        first_output = now;
      }
      last_output = now;
      ++output_count;
    }
    condition.notify_all();
  });

  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, MillisecondStreams());
    for (std::int64_t dts = 0; dts <= 1400; dts += 100) {
      Feed(*queue, 1, 0, dts);
      Feed(*queue, 1, 1, dts);
    }
    queue->EndInput(1);
  });

  REQUIRE(WaitFor(
      condition, mutex, [&]() { return output_count.load() == 30; }, 3s));
  RunOnPollerAndWait(queue->poller(), []() {});

  std::chrono::milliseconds output_span;
  {
    std::lock_guard<std::mutex> lock(mutex);
    output_span = std::chrono::duration_cast<std::chrono::milliseconds>(
        last_output - first_output);
  }
  CHECK(output_span >= 1250ms);
  CHECK(output_span < 2200ms);
  CHECK(queue->packet_count() == 0);
  CHECK(queue->state() == PacketQueueState::kStarved);

  std::this_thread::sleep_for(300ms);
  CHECK(output_count == 30);

  StopAndWait(queue);
}

TEST_CASE("packet queue interleaves normalized DTS without rewriting packets") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;

  struct Output {
    int stream_index;
    std::int64_t dts;
    AVRational time_base;
    std::int64_t dts_us;
  };
  std::vector<Output> outputs;

  queue->SetOnPacket([&](std::uint64_t, const AVPacket* packet) {
    const auto dts_us =
        av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q);
    {
      std::lock_guard<std::mutex> lock(mutex);
      outputs.push_back(
          {packet->stream_index, packet->dts, packet->time_base, dts_us});
    }
    condition.notify_all();
  });

  const AVRational video_time_base{1, 1000};
  const AVRational audio_time_base{1, 48000};
  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, {{0, AVMEDIA_TYPE_VIDEO, video_time_base},
                          {1, AVMEDIA_TYPE_AUDIO, audio_time_base}});

    for (std::int64_t index = 0; index <= 12; ++index) {
      Feed(*queue, 1, 0, 10 + index * 100, video_time_base);
    }
    for (std::int64_t index = 0; index <= 30; ++index) {
      Feed(*queue, 1, 1, index * 1920, audio_time_base);
    }
    queue->EndInput(1);
  });

  REQUIRE(WaitFor(
      condition, mutex, [&]() { return outputs.size() == 44; }, 3s));

  std::vector<Output> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    snapshot = outputs;
  }
  REQUIRE(snapshot.size() == 44);
  for (std::size_t index = 1; index < snapshot.size(); ++index) {
    CHECK(snapshot[index - 1].dts_us <= snapshot[index].dts_us);
  }
  for (const auto& output : snapshot) {
    if (output.stream_index == 0) {
      CHECK(output.time_base.num == video_time_base.num);
      CHECK(output.time_base.den == video_time_base.den);
      CHECK(output.dts % 100 == 10);
    } else {
      CHECK(output.stream_index == 1);
      CHECK(output.time_base.num == audio_time_base.num);
      CHECK(output.time_base.den == audio_time_base.den);
      CHECK(output.dts % 1920 == 0);
    }
  }

  StopAndWait(queue);
}

TEST_CASE("new generation atomically clears the old packet timeline") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t generation_one_outputs = 0;
  std::atomic_size_t generation_two_outputs = 0;
  std::atomic_size_t timeline_resets = 0;
  std::atomic_bool reset_seen = false;
  std::atomic_bool old_packet_after_reset = false;
  std::atomic_bool reset_cleared_packets = false;

  queue->SetOnTimelineReset([&](std::uint64_t generation) {
    if (generation == 2) {
      reset_cleared_packets = queue->packet_count() == 0;
      reset_seen = true;
      ++timeline_resets;
    }
  });
  queue->SetOnPacket([&](std::uint64_t generation, const AVPacket*) {
    if (generation == 1) {
      if (reset_seen) {
        old_packet_after_reset = true;
      }
      ++generation_one_outputs;
    } else if (generation == 2) {
      ++generation_two_outputs;
    }
    condition.notify_all();
  });

  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, MillisecondStreams());
    for (std::int64_t dts = 0; dts <= 2000; dts += 100) {
      Feed(*queue, 1, 0, dts);
      Feed(*queue, 1, 1, dts);
    }
  });
  REQUIRE(WaitFor(condition, mutex,
                  [&]() { return generation_one_outputs.load() >= 4; }));

  RunOnPollerAndWait(queue->poller(), [&]() {
    Feed(*queue, 2, 0, 0);
    Feed(*queue, 1, 0, 2100);
    Feed(*queue, 2, 1, 0);
    for (std::int64_t dts = 100; dts <= 1200; dts += 100) {
      Feed(*queue, 2, 0, dts);
      Feed(*queue, 2, 1, dts);
    }
    queue->EndInput(2);
  });

  REQUIRE(WaitFor(
      condition, mutex, [&]() { return generation_two_outputs.load() == 26; },
      3s));
  RunOnPollerAndWait(queue->poller(), []() {});

  CHECK(reset_seen);
  CHECK(reset_cleared_packets);
  CHECK(timeline_resets == 1);
  CHECK_FALSE(old_packet_after_reset);
  CHECK(generation_one_outputs < 42);
  CHECK(queue->generation() == 2);
  CHECK(queue->packet_count() == 0);
  CHECK(queue->state() == PacketQueueState::kStarved);

  StopAndWait(queue);
}

TEST_CASE("packet queue validates its supported stream combinations") {
  CHECK_THROWS_AS(std::make_shared<PacketQueue>(999ms), std::invalid_argument);
  CHECK_THROWS_AS(std::make_shared<PacketQueue>(30001ms),
                  std::invalid_argument);

  auto queue = std::make_shared<PacketQueue>(1s);
  CHECK_THROWS_AS(queue->SetStreams(1, {}), std::invalid_argument);
  CHECK_NOTHROW(queue->SetStreams(1, {{0, AVMEDIA_TYPE_VIDEO, {1, 1000}}}));
  CHECK_NOTHROW(queue->SetStreams(2, {{1, AVMEDIA_TYPE_AUDIO, {1, 1000}}}));
  CHECK_NOTHROW(queue->SetStreams(3, MillisecondStreams()));
  CHECK_THROWS_AS(queue->SetStreams(4, {{0, AVMEDIA_TYPE_VIDEO, {1, 1000}},
                                        {1, AVMEDIA_TYPE_VIDEO, {1, 1000}}}),
                  std::invalid_argument);
  CHECK_THROWS_AS(queue->SetStreams(4, {{0, AVMEDIA_TYPE_AUDIO, {1, 1000}},
                                        {1, AVMEDIA_TYPE_AUDIO, {1, 1000}}}),
                  std::invalid_argument);
  CHECK_THROWS_AS(queue->SetStreams(1, {{0, AVMEDIA_TYPE_VIDEO, {1, 1000}},
                                        {0, AVMEDIA_TYPE_AUDIO, {1, 1000}}}),
                  std::invalid_argument);
  CHECK_THROWS_AS(queue->SetStreams(1, {{0, AVMEDIA_TYPE_DATA, {1, 1000}}}),
                  std::invalid_argument);
  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(4, MillisecondStreams());
    Feed(*queue, 4, 0, 0);
    Feed(*queue, 4, 1, 0);
    queue->SetStreams(4, MillisecondStreams());
  });
  CHECK(queue->packet_count() == 2);
  StopAndWait(queue);
}

TEST_CASE("new generation replaces the configured track set") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<int> output_stream_indexes;

  queue->SetPlaybackRate(20.0);
  queue->SetOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    if (generation == 2) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        output_stream_indexes.push_back(packet->stream_index);
      }
      condition.notify_all();
    }
  });

  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, MillisecondStreams());
    Feed(*queue, 1, 0, 0);
    Feed(*queue, 1, 1, 0);

    queue->SetStreams(2, {{5, AVMEDIA_TYPE_AUDIO, {1, 1000}}});
    Feed(*queue, 2, 0, 0);
    for (std::int64_t dts = 0; dts <= 1000; dts += 100) {
      Feed(*queue, 2, 5, dts);
    }
    queue->EndInput(2);
  });

  REQUIRE(WaitFor(condition, mutex,
                  [&]() { return output_stream_indexes.size() == 11; }));
  RunOnPollerAndWait(queue->poller(), []() {});

  std::vector<int> output_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    output_snapshot = output_stream_indexes;
  }
  REQUIRE(output_snapshot.size() == 11);
  for (const auto stream_index : output_snapshot) {
    CHECK(stream_index == 5);
  }
  CHECK(queue->packet_count() == 0);
  CHECK(queue->state() == PacketQueueState::kStarved);

  StopAndWait(queue);
}

TEST_CASE("packet queue pause and rate control its output clock") {
  auto queue = std::make_shared<PacketQueue>(1s);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t output_count = 0;

  queue->SetPlaybackRate(4.0);
  queue->SetOnPacket([&](std::uint64_t, const AVPacket*) {
    ++output_count;
    condition.notify_all();
  });
  RunOnPollerAndWait(queue->poller(), [&]() {
    queue->SetStreams(1, MillisecondStreams());
    for (std::int64_t dts = 0; dts <= 2000; dts += 100) {
      Feed(*queue, 1, 0, dts);
      Feed(*queue, 1, 1, dts);
    }
    queue->EndInput(1);
  });

  REQUIRE(
      WaitFor(condition, mutex, [&]() { return output_count.load() >= 4; }));
  queue->Pause(true);
  RunOnPollerAndWait(queue->poller(), []() {});
  const auto paused_count = output_count.load();

  std::this_thread::sleep_for(250ms);
  CHECK(output_count == paused_count);
  CHECK(queue->state() == PacketQueueState::kPaused);

  queue->SetPlaybackRate(10.0);
  queue->Pause(false);
  REQUIRE(WaitFor(
      condition, mutex, [&]() { return output_count.load() == 42; }, 2s));
  RunOnPollerAndWait(queue->poller(), []() {});
  CHECK(queue->state() == PacketQueueState::kStarved);
  CHECK_THROWS_AS(queue->SetPlaybackRate(0.0), std::invalid_argument);
  CHECK_THROWS_AS(queue->SetPlaybackRate(21.0), std::invalid_argument);

  StopAndWait(queue);
}

TEST_CASE(
    "packet queue survives callback reentrancy and synchronous disposal") {
  SECTION("OnPacket can stop the queue") {
    auto queue = std::make_shared<PacketQueue>(1s);
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_size_t output_count = 0;
    std::weak_ptr<PacketQueue> weak_queue = queue;

    queue->SetOnPacket([&](std::uint64_t, const AVPacket*) {
      if (++output_count == 1) {
        if (auto locked = weak_queue.lock()) {
          locked->Stop();
        }
      }
      condition.notify_all();
    });
    RunOnPollerAndWait(queue->poller(), [&]() {
      queue->SetStreams(1, MillisecondStreams());
      for (std::int64_t dts = 0; dts <= 1400; dts += 100) {
        Feed(*queue, 1, 0, dts);
        Feed(*queue, 1, 1, dts);
      }
      queue->EndInput(1);
    });

    REQUIRE(
        WaitFor(condition, mutex, [&]() { return output_count.load() == 1; }));
    RunOnPollerAndWait(queue->poller(), []() {});
    CHECK(queue->state() == PacketQueueState::kStopped);
    CHECK(queue->packet_count() == 0);
    std::this_thread::sleep_for(200ms);
    CHECK(output_count == 1);
  }

  SECTION("destruction suppresses all pending callbacks") {
    auto queue = std::make_shared<PacketQueue>(1s);
    auto poller = queue->poller();
    std::atomic_size_t output_count = 0;

    queue->SetOnPacket([&](std::uint64_t, const AVPacket*) { ++output_count; });
    RunOnPollerAndWait(poller, [&]() {
      queue->SetStreams(1, MillisecondStreams());
      for (std::int64_t dts = 0; dts <= 3000; dts += 100) {
        Feed(*queue, 1, 0, dts);
        Feed(*queue, 1, 1, dts);
      }
      queue->EndInput(1);
    });

    queue.reset();
    const auto count_after_destruction = output_count.load();
    RunOnPollerAndWait(poller, []() {});
    std::this_thread::sleep_for(200ms);
    CHECK(output_count == count_after_destruction);
  }

  SECTION("the last owner can be released inside OnPacket") {
    auto queue = std::make_shared<PacketQueue>(1s);
    auto poller = queue->poller();
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic_bool callback_completed = false;

    queue->SetOnPacket([&](std::uint64_t, const AVPacket*) {
      queue.reset();
      callback_completed = true;
      condition.notify_all();
    });
    RunOnPollerAndWait(poller, [&]() {
      queue->SetStreams(1, MillisecondStreams());
      for (std::int64_t dts = 0; dts <= 1400; dts += 100) {
        Feed(*queue, 1, 0, dts);
        Feed(*queue, 1, 1, dts);
      }
      queue->EndInput(1);
    });

    REQUIRE(
        WaitFor(condition, mutex, [&]() { return callback_completed.load(); }));
    RunOnPollerAndWait(poller, []() {});
    CHECK_FALSE(queue);
  }
}

TEST_CASE("player proxy feeds and fully drains an eight second packet queue") {
  auto poller = toolkit::EventPollerPool::Instance().getPoller();
  auto queue = std::make_shared<PacketQueue>(1s, poller);
  auto proxy = std::make_shared<PlayerProxy>(poller);
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::int64_t> normalized_dts;
  std::atomic_size_t video_packets = 0;
  std::atomic_size_t audio_packets = 0;
  std::atomic_size_t output_at_input_eof = 0;
  std::atomic_bool input_ended = false;
  std::atomic_bool callbacks_on_poller = true;

  queue->SetOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    callbacks_on_poller =
        callbacks_on_poller && generation == 1 && poller->isCurrentThread();
    {
      std::lock_guard<std::mutex> lock(mutex);
      normalized_dts.push_back(
          av_rescale_q(packet->dts, packet->time_base, AV_TIME_BASE_Q));
    }
    if (packet->stream_index == 0) {
      ++video_packets;
    } else if (packet->stream_index == 1) {
      ++audio_packets;
    }
    condition.notify_all();
  });
  proxy->SetOnStreamsReady(
      [&](std::uint64_t generation,
          const std::vector<mw::streamer::input::StreamInfo>& streams) {
        std::vector<PacketStream> packet_streams;
        for (const auto& stream : streams) {
          packet_streams.push_back({stream.stream_index,
                                    stream.codec_parameters->codec_type,
                                    stream.time_base});
        }
        queue->SetStreams(generation, std::move(packet_streams));
      });
  proxy->SetOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    return queue->Input(generation, packet);
  });
  proxy->SetOnState([&](std::uint64_t generation, PlayerState state,
                        const toolkit::SockException&, bool) {
    if (state == PlayerState::kEnded) {
      queue->EndInput(generation);
      output_at_input_eof = video_packets.load() + audio_packets.load();
      input_ended = true;
      condition.notify_all();
    }
  });

  proxy->Start(SamplePath());

  REQUIRE(WaitFor(
      condition, mutex, [&]() { return input_ended.load(); }, 12s));
  CHECK(output_at_input_eof < 456);
  REQUIRE(WaitFor(
      condition, mutex,
      [&]() {
        return video_packets.load() == 80 && audio_packets.load() == 376;
      },
      12s));
  RunOnPollerAndWait(poller, []() {});

  std::vector<std::int64_t> dts_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    dts_snapshot = normalized_dts;
  }
  REQUIRE(dts_snapshot.size() == 456);
  for (std::size_t index = 1; index < dts_snapshot.size(); ++index) {
    CHECK(dts_snapshot[index - 1] <= dts_snapshot[index]);
  }
  CHECK(callbacks_on_poller);
  CHECK(queue->state() == PacketQueueState::kStarved);
  CHECK(queue->packet_count() == 0);

  StopAndWait(queue);

  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool stopped = false;
  proxy->Stop([&]() {
    {
      std::lock_guard<std::mutex> lock(stop_mutex);
      stopped = true;
    }
    stop_condition.notify_all();
  });
  REQUIRE(WaitFor(stop_condition, stop_mutex, [&]() { return stopped; }));
}

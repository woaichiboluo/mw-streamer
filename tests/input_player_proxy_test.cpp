#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "Poller/EventPoller.h"
#include "mw/input/PlayerProxy.h"

namespace {

using namespace std::chrono_literals;
using mw::input::ControlResult;
using mw::input::PlayerProxy;
using mw::input::PlayerState;
using mw::input::ReconnectPolicy;
using mw::input::TimelineResetReason;
using toolkit::Err_eof;
using toolkit::Err_other;
using toolkit::ErrCode;
using toolkit::SockException;

std::string samplePath(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_INPUT_PLAYER_PROXY_TEST_DATA_DIR) + "/" + name;
}

bool waitFor(std::condition_variable& condition, std::mutex& mutex,
             const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = 5s) {
  std::unique_lock<std::mutex> lock(mutex);
  return condition.wait_for(lock, timeout, predicate);
}

void stopAndWait(const PlayerProxy::Ptr& proxy) {
  std::mutex mutex;
  std::condition_variable condition;
  bool stopped = false;
  proxy->stop([&]() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopped = true;
    }
    condition.notify_all();
  });
  REQUIRE(waitFor(condition, mutex, [&]() { return stopped; }));
}

}  // namespace

TEST_CASE(
    "input player proxy publishes streams and AVPackets without a "
    "MediaSource") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<AVMediaType> stream_types;
  std::atomic_size_t video_packets = 0;
  std::atomic_size_t audio_packets = 0;
  std::atomic_bool valid_packets = true;
  std::atomic_bool ready = false;
  std::atomic_bool ended = false;
  std::atomic<ErrCode> end_reason = Err_other;

  proxy->setOnStreamsReady(
      [&](std::uint64_t generation,
          const std::vector<mw::input::StreamInfo>& streams) {
        if (generation != 1) {
          valid_packets = false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& stream : streams) {
          if (!stream.codec_parameters || stream.stream_index < 0 ||
              stream.time_base.num != 1 || stream.time_base.den != 1000) {
            valid_packets = false;
            continue;
          }
          stream_types.emplace_back(stream.codec_parameters->codec_type);
        }
      });
  proxy->setOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    if (generation != 1 || !packet || !packet->buf || !packet->data ||
        packet->size <= 0 || packet->dts == AV_NOPTS_VALUE ||
        packet->pts == AV_NOPTS_VALUE || packet->time_base.num != 1 ||
        packet->time_base.den != 1000) {
      valid_packets = false;
      return false;
    }
    if (packet->stream_index == 0) {
      ++video_packets;
    } else if (packet->stream_index == 1) {
      ++audio_packets;
    } else {
      valid_packets = false;
    }
    return true;
  });
  proxy->setOnState([&](std::uint64_t generation, PlayerState state,
                        const SockException& reason, bool will_retry) {
    if (generation != 1 || will_retry) {
      valid_packets = false;
    }
    if (state == PlayerState::Ready) {
      ready = true;
    }
    if (state == PlayerState::Ended) {
      end_reason = reason.getErrCode();
      ended = true;
      condition.notify_all();
    }
  });

  proxy->start(samplePath());

  REQUIRE(waitFor(condition, mutex, [&]() { return ended.load(); }));
  CHECK(ready);
  CHECK(end_reason == Err_eof);
  CHECK(proxy->generation() == 1);
  CHECK(proxy->reconnectCount() == 0);
  CHECK(valid_packets);
  CHECK(video_packets == 20);
  CHECK(audio_packets == 95);
  REQUIRE(stream_types.size() == 2);
  CHECK(stream_types[0] == AVMEDIA_TYPE_VIDEO);
  CHECK(stream_types[1] == AVMEDIA_TYPE_AUDIO);

  stopAndWait(proxy);
}

TEST_CASE("input player proxy does not retry a failed finite input") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool failed = false;
  std::atomic_bool retried = false;
  std::atomic_size_t streams_ready = 0;
  std::atomic_size_t packets = 0;

  proxy->setOnStreamsReady(
      [&](std::uint64_t, const std::vector<mw::input::StreamInfo>&) {
        ++streams_ready;
      });
  proxy->setOnPacket([&](std::uint64_t, const AVPacket*) {
    ++packets;
    return true;
  });
  proxy->setOnState([&](std::uint64_t, PlayerState state, const SockException&,
                        bool will_retry) {
    retried = retried || will_retry;
    if (state == PlayerState::Failed) {
      failed = true;
      condition.notify_all();
    }
  });

  proxy->start(samplePath() + ".missing");

  REQUIRE(waitFor(condition, mutex, [&]() { return failed.load(); }));
  CHECK_FALSE(retried);
  CHECK(streams_ready == 0);
  CHECK(packets == 0);
  CHECK(proxy->generation() == 1);
  CHECK(proxy->reconnectCount() == 0);

  stopAndWait(proxy);
}

TEST_CASE(
    "input player proxy retries a failed live input with the configured "
    "budget") {
  ReconnectPolicy policy;
  policy.max_retries = 1;
  policy.min_delay = 20ms;
  policy.max_delay = 20ms;
  policy.delay_step = 20ms;
  auto proxy = std::make_shared<PlayerProxy>(nullptr, policy);

  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t waiting_retry = 0;
  std::atomic_bool failed = false;

  proxy->setOnState([&](std::uint64_t, PlayerState state, const SockException&,
                        bool will_retry) {
    if (state == PlayerState::WaitingRetry && will_retry) {
      ++waiting_retry;
    }
    if (state == PlayerState::Failed) {
      failed = true;
      condition.notify_all();
    }
  });

  proxy->start("rtsp://127.0.0.1:1/mw-unreachable");

  REQUIRE(waitFor(
      condition, mutex, [&]() { return failed.load(); }, 3s));
  CHECK(waiting_retry == 1);
  CHECK(proxy->generation() == 2);
  CHECK(proxy->reconnectCount() == 1);

  stopAndWait(proxy);
}

TEST_CASE("stopping input player proxy cancels a pending reconnect") {
  ReconnectPolicy policy;
  policy.max_retries = -1;
  policy.min_delay = 2s;
  policy.max_delay = 2s;
  policy.delay_step = 2s;
  auto proxy = std::make_shared<PlayerProxy>(nullptr, policy);

  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool waiting = false;

  proxy->setOnState([&](std::uint64_t, PlayerState state, const SockException&,
                        bool will_retry) {
    if (state == PlayerState::WaitingRetry && will_retry) {
      waiting = true;
      condition.notify_all();
    }
  });
  proxy->start("rtsp://127.0.0.1:1/mw-stop-reconnect");

  REQUIRE(waitFor(
      condition, mutex, [&]() { return waiting.load(); }, 3s));
  const auto generation_before_stop = proxy->generation();
  stopAndWait(proxy);
  std::this_thread::sleep_for(2100ms);

  CHECK(proxy->state() == PlayerState::Stopped);
  CHECK(proxy->generation() == generation_before_stop);
  CHECK(proxy->reconnectCount() == 1);
}

TEST_CASE("destroying input player proxy cleans up without user callbacks") {
  ReconnectPolicy policy;
  policy.max_retries = -1;
  policy.min_delay = 2s;
  policy.max_delay = 2s;
  policy.delay_step = 2s;
  auto proxy = std::make_shared<PlayerProxy>(nullptr, policy);

  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t callback_count = 0;
  std::atomic_bool waiting = false;

  proxy->setOnState([&](std::uint64_t, PlayerState state, const SockException&,
                        bool will_retry) {
    ++callback_count;
    if (state == PlayerState::WaitingRetry && will_retry) {
      waiting = true;
      condition.notify_all();
    }
  });
  proxy->start("rtsp://127.0.0.1:1/mw-destroy");

  REQUIRE(waitFor(
      condition, mutex, [&]() { return waiting.load(); }, 3s));
  const auto count_before_destroy = callback_count.load();
  auto poller = proxy->getPoller();
  proxy.reset();

  bool barrier_reached = false;
  poller->async(
      [&]() {
        {
          std::lock_guard<std::mutex> lock(mutex);
          barrier_reached = true;
        }
        condition.notify_all();
      },
      false);

  REQUIRE(waitFor(condition, mutex, [&]() { return barrier_reached; }));
  CHECK(callback_count == count_before_destroy);
}

TEST_CASE(
    "file controls pause and resume packet delivery on the owner poller") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t packet_count = 0;
  std::atomic_bool ready = false;
  std::atomic_bool pause_completed = false;
  std::atomic_bool resume_completed = false;
  std::atomic_bool callbacks_on_poller = true;
  std::atomic<ControlResult> pause_result = ControlResult::Failed;
  std::atomic<ControlResult> resume_result = ControlResult::Failed;

  proxy->setOnPacket([&](std::uint64_t, const AVPacket*) {
    ++packet_count;
    condition.notify_all();
    return true;
  });
  proxy->setOnState(
      [&](std::uint64_t, PlayerState state, const SockException&, bool) {
        if (state == PlayerState::Ready) {
          ready = true;
          condition.notify_all();
        }
      });
  proxy->start(samplePath());

  REQUIRE(waitFor(condition, mutex, [&]() { return ready.load(); }));
  proxy->pause(true, [&](ControlResult result, std::uint64_t generation) {
    pause_result = result;
    callbacks_on_poller =
        callbacks_on_poller && proxy->getPoller()->isCurrentThread();
    if (generation != 1) {
      callbacks_on_poller = false;
    }
    pause_completed = true;
    condition.notify_all();
  });
  REQUIRE(waitFor(condition, mutex, [&]() { return pause_completed.load(); }));

  const auto paused_packet_count = packet_count.load();
  std::this_thread::sleep_for(400ms);
  CHECK(packet_count == paused_packet_count);
  CHECK(pause_result == ControlResult::Accepted);
  CHECK(proxy->generation() == 1);

  proxy->pause(false, [&](ControlResult result, std::uint64_t generation) {
    resume_result = result;
    callbacks_on_poller =
        callbacks_on_poller && proxy->getPoller()->isCurrentThread();
    if (generation != 1) {
      callbacks_on_poller = false;
    }
    resume_completed = true;
    condition.notify_all();
  });
  REQUIRE(waitFor(condition, mutex, [&]() { return resume_completed.load(); }));
  REQUIRE(waitFor(condition, mutex,
                  [&]() { return packet_count.load() > paused_packet_count; }));
  CHECK(resume_result == ControlResult::Accepted);
  CHECK(callbacks_on_poller);
  CHECK(proxy->generation() == 1);

  stopAndWait(proxy);
}

TEST_CASE("file seek starts a clean timeline generation") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool ready = false;
  std::atomic_bool paused = false;
  std::atomic_bool seek_completed = false;
  std::atomic_bool reset_seen = false;
  std::atomic_bool ended = false;
  std::atomic_bool valid_timeline = true;
  std::atomic_size_t generation_two_video_packets = 0;
  std::atomic<std::int64_t> first_video_dts = AV_NOPTS_VALUE;
  std::atomic_int first_video_flags = 0;
  std::atomic<ControlResult> seek_result = ControlResult::Failed;

  proxy->setOnTimelineReset([&](std::uint64_t generation,
                                TimelineResetReason reason,
                                std::chrono::milliseconds position) {
    if (generation != 2 || reason != TimelineResetReason::Seek ||
        position != 1000ms || !proxy->getPoller()->isCurrentThread()) {
      valid_timeline = false;
    }
    reset_seen = true;
  });
  proxy->setOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    if (seek_completed && generation != 2) {
      valid_timeline = false;
    }
    if (generation == 2) {
      if (!reset_seen) {
        valid_timeline = false;
      }
      if (packet->stream_index == 0) {
        ++generation_two_video_packets;
        auto expected = AV_NOPTS_VALUE;
        if (first_video_dts.compare_exchange_strong(expected, packet->dts)) {
          first_video_flags = packet->flags;
        }
      }
    }
    return true;
  });
  proxy->setOnState([&](std::uint64_t generation, PlayerState state,
                        const SockException& reason, bool) {
    if (state == PlayerState::Ready) {
      ready = true;
      condition.notify_all();
    }
    if (state == PlayerState::Ended) {
      if (generation != 2 || reason.getErrCode() != Err_eof) {
        valid_timeline = false;
      }
      ended = true;
      condition.notify_all();
    }
  });
  proxy->start(samplePath());

  REQUIRE(waitFor(condition, mutex, [&]() { return ready.load(); }));
  proxy->pause(true, [&](ControlResult result, std::uint64_t) {
    if (result != ControlResult::Accepted) {
      valid_timeline = false;
    }
    paused = true;
    condition.notify_all();
  });
  REQUIRE(waitFor(condition, mutex, [&]() { return paused.load(); }));

  proxy->seekTo(1000ms, [&](ControlResult result, std::uint64_t generation) {
    seek_result = result;
    if (generation != 2 || !proxy->getPoller()->isCurrentThread()) {
      valid_timeline = false;
    }
    seek_completed = true;
    condition.notify_all();
  });
  REQUIRE(waitFor(condition, mutex, [&]() { return seek_completed.load(); }));
  proxy->setPlaybackRate(20.0f);

  REQUIRE(waitFor(condition, mutex, [&]() { return ended.load(); }));
  CHECK(seek_result == ControlResult::Accepted);
  CHECK(reset_seen);
  CHECK(valid_timeline);
  CHECK(proxy->generation() == 2);
  CHECK(proxy->reconnectCount() == 0);
  CHECK(generation_two_video_packets > 0);
  CHECK(first_video_dts >= 1000);
  CHECK(first_video_dts <= 1100);
  CHECK((first_video_flags.load() & AV_PKT_FLAG_KEY) != 0);

  stopAndWait(proxy);
}

TEST_CASE("file playback rate changes pacing without changing generation") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t video_packets = 0;
  std::atomic_size_t audio_packets = 0;
  std::atomic_bool ready = false;
  std::atomic_bool rate_completed = false;
  std::atomic_bool ended = false;
  std::atomic_bool reset_seen = false;
  std::atomic<ControlResult> rate_result = ControlResult::Failed;

  proxy->setOnTimelineReset(
      [&](std::uint64_t, TimelineResetReason, std::chrono::milliseconds) {
        reset_seen = true;
      });
  proxy->setOnPacket([&](std::uint64_t generation, const AVPacket* packet) {
    if (generation == 1 && packet->stream_index == 0) {
      ++video_packets;
    } else if (generation == 1 && packet->stream_index == 1) {
      ++audio_packets;
    }
    return true;
  });
  proxy->setOnState(
      [&](std::uint64_t, PlayerState state, const SockException&, bool) {
        if (state == PlayerState::Ready) {
          ready = true;
          condition.notify_all();
        }
        if (state == PlayerState::Ended) {
          ended = true;
          condition.notify_all();
        }
      });
  proxy->start(samplePath());

  REQUIRE(waitFor(condition, mutex, [&]() { return ready.load(); }));
  const auto rate_started = std::chrono::steady_clock::now();
  proxy->setPlaybackRate(
      20.0f, [&](ControlResult result, std::uint64_t generation) {
        rate_result = result;
        if (generation != 1 || !proxy->getPoller()->isCurrentThread()) {
          rate_result = ControlResult::Failed;
        }
        rate_completed = true;
        condition.notify_all();
      });
  REQUIRE(waitFor(condition, mutex, [&]() { return rate_completed.load(); }));
  REQUIRE(waitFor(condition, mutex, [&]() { return ended.load(); }));
  const auto elapsed = std::chrono::steady_clock::now() - rate_started;

  CHECK(rate_result == ControlResult::Accepted);
  CHECK(proxy->generation() == 1);
  CHECK_FALSE(reset_seen);
  CHECK(video_packets == 20);
  CHECK(audio_packets == 95);
  CHECK(elapsed < 1500ms);

  stopAndWait(proxy);
}

TEST_CASE("playback controls reject invalid state and arguments") {
  auto proxy = std::make_shared<PlayerProxy>();
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<ControlResult> results;
  std::atomic_bool callbacks_on_poller = true;

  auto completed = [&](ControlResult result, std::uint64_t generation) {
    callbacks_on_poller =
        callbacks_on_poller && proxy->getPoller()->isCurrentThread();
    if (generation != 0) {
      callbacks_on_poller = false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      results.emplace_back(result);
    }
    condition.notify_all();
  };

  proxy->pause(true, completed);
  proxy->seekTo(1s, completed);
  proxy->setPlaybackRate(2.0f, completed);
  proxy->seekTo(-1ms, completed);
  proxy->setPlaybackRate(std::numeric_limits<float>::infinity(), completed);

  REQUIRE(waitFor(condition, mutex, [&]() { return results.size() == 5; }));
  REQUIRE(results.size() == 5);
  CHECK(results[0] == ControlResult::InvalidState);
  CHECK(results[1] == ControlResult::InvalidState);
  CHECK(results[2] == ControlResult::InvalidState);
  CHECK(results[3] == ControlResult::InvalidArgument);
  CHECK(results[4] == ControlResult::InvalidArgument);
  CHECK(callbacks_on_poller);
  CHECK(proxy->state() == PlayerState::Idle);
  CHECK(proxy->generation() == 0);
}

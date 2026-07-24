#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Player/FilePlayerImp.h"
#include "Player/MediaPlayer.h"
#include "Player/PlayerProxy.h"
#include "Util/onceToken.h"

namespace {

using namespace std::chrono_literals;
using mediakit::FilePlayerImp;
using mediakit::MediaPlayer;
using mediakit::PlayerBase;
using mediakit::PlayerProxy;
using mediakit::CodecAAC;
using mediakit::CodecH264;
using mediakit::CodecH265;
using mediakit::CodecId;
using toolkit::Err_eof;
using toolkit::Err_other;
using toolkit::Err_success;
using toolkit::ErrCode;
using toolkit::SockException;

std::string samplePath(const std::string &name = "file_player.mp4") {
  return std::string(MW_FILE_PLAYER_TEST_DATA_DIR) + "/" + name;
}

struct Sample {
  const char *name;
  const char *file;
  CodecId video_codec;
  bool has_audio;
};

const std::vector<Sample> kSamples = {
    {"H.264 + AAC MP4", "h264_aac.mp4", CodecH264, true},
    {"H.265 + AAC MP4", "h265_aac.mp4", CodecH265, true},
    {"H.264 video-only MP4", "h264_video.mp4", CodecH264, false},
    {"H.265 video-only MP4", "h265_video.mp4", CodecH265, false},
    {"H.264 + AAC fragmented MP4", "h264_aac_fragmented.mp4", CodecH264,
     true},
    {"H.265 + AAC fragmented MP4", "h265_aac_fragmented.mp4", CodecH265,
     true},
};

struct PlaybackObservation {
  bool play_called = false;
  ErrCode play_result = Err_other;
  bool finished = false;
  ErrCode shutdown_result = Err_other;
  std::vector<CodecId> codecs;
  size_t frame_count = 0;
  size_t h264_frame_count = 0;
  size_t h265_frame_count = 0;
  size_t aac_frame_count = 0;
  float duration = 0;
  std::chrono::steady_clock::duration elapsed{};
};

PlaybackObservation playToEnd(const std::string &path, float speed,
                              std::chrono::milliseconds timeout = 5s) {
  MediaPlayer player;
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool play_called = false;
  std::atomic<ErrCode> play_result = Err_other;
  std::atomic_bool shutdown_called = false;
  std::atomic<ErrCode> shutdown_result = Err_other;
  std::atomic_size_t frame_count = 0;
  std::atomic_size_t h264_frame_count = 0;
  std::atomic_size_t h265_frame_count = 0;
  std::atomic_size_t aac_frame_count = 0;
  std::vector<CodecId> codecs;
  float duration = 0;

  player.setOnPlayResult([&](const SockException &ex) {
    play_result = ex.getErrCode();
    play_called = true;
    if (ex) {
      condition.notify_all();
      return;
    }
    duration = player.getDuration();
    for (auto &track : player.getTracks(false)) {
      codecs.emplace_back(track->getCodecId());
      track->addDelegate([&](const mediakit::Frame::Ptr &frame) {
        ++frame_count;
        switch (frame->getCodecId()) {
          case CodecH264:
            ++h264_frame_count;
            break;
          case CodecH265:
            ++h265_frame_count;
            break;
          case CodecAAC:
            ++aac_frame_count;
            break;
          default:
            break;
        }
        return true;
      });
    }
    player.speed(speed);
  });
  player.setOnShutdown([&](const SockException &ex) {
    shutdown_result = ex.getErrCode();
    shutdown_called = true;
    condition.notify_all();
  });

  const auto begin = std::chrono::steady_clock::now();
  player.play(path);
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, timeout, [&]() {
      return shutdown_called.load() ||
             (play_called.load() && play_result.load() != Err_success);
    });
  }

  PlaybackObservation observation;
  observation.play_called = play_called;
  observation.play_result = play_result;
  observation.finished = shutdown_called;
  observation.shutdown_result = shutdown_result;
  observation.codecs = std::move(codecs);
  observation.frame_count = frame_count;
  observation.h264_frame_count = h264_frame_count;
  observation.h265_frame_count = h265_frame_count;
  observation.aac_frame_count = aac_frame_count;
  observation.duration = duration;
  observation.elapsed = std::chrono::steady_clock::now() - begin;
  return observation;
}

}  // namespace

TEST_CASE("plain MP4 path creates a finite file player") {
  auto player = PlayerBase::createPlayer(nullptr, samplePath());

  REQUIRE(std::dynamic_pointer_cast<FilePlayerImp>(player));
  CHECK(player->isFinite());
  CHECK_THROWS(PlayerBase::createPlayer(nullptr, "unknown://source"));
}

TEST_CASE("file player demuxes the supported MP4 sample matrix") {
  for (const auto &sample : kSamples) {
    DYNAMIC_SECTION(sample.name) {
      auto observation = playToEnd(samplePath(sample.file), 20.0f);

      REQUIRE(observation.play_called);
      REQUIRE(observation.play_result == Err_success);
      REQUIRE(observation.finished);
      CHECK(observation.shutdown_result == Err_eof);
      CHECK(observation.duration >= 1.9f);
      CHECK(observation.frame_count > 0);
      CHECK((sample.video_codec == CodecH264 ? observation.h264_frame_count
                                             : observation.h265_frame_count) >
            0);
      CHECK((observation.aac_frame_count > 0) == sample.has_audio);
      CHECK(std::count(observation.codecs.begin(), observation.codecs.end(),
                       sample.video_codec) == 1);
      CHECK(std::count(observation.codecs.begin(), observation.codecs.end(),
                       CodecAAC) == (sample.has_audio ? 1 : 0));
      CHECK(observation.codecs.size() == (sample.has_audio ? 2 : 1));
    }
  }
}

TEST_CASE("file player speed controls MP4 demux pacing") {
  auto normal = playToEnd(samplePath("h264_aac.mp4"), 1.0f);
  auto double_speed = playToEnd(samplePath("h264_aac.mp4"), 2.0f);
  auto twenty_speed = playToEnd(samplePath("h264_aac.mp4"), 20.0f);

  REQUIRE(normal.finished);
  REQUIRE(double_speed.finished);
  REQUIRE(twenty_speed.finished);
  REQUIRE(normal.shutdown_result == Err_eof);
  REQUIRE(double_speed.shutdown_result == Err_eof);
  REQUIRE(twenty_speed.shutdown_result == Err_eof);

  CHECK(normal.elapsed > double_speed.elapsed + 300ms);
  CHECK(double_speed.elapsed > twenty_speed.elapsed + 250ms);

  auto h265_normal =
      playToEnd(samplePath("h265_aac_fragmented.mp4"), 1.0f);
  auto h265_twenty =
      playToEnd(samplePath("h265_aac_fragmented.mp4"), 20.0f);

  REQUIRE(h265_normal.finished);
  REQUIRE(h265_twenty.finished);
  REQUIRE(h265_normal.shutdown_result == Err_eof);
  REQUIRE(h265_twenty.shutdown_result == Err_eof);
  CHECK(h265_normal.elapsed > h265_twenty.elapsed + 800ms);
}

TEST_CASE("file player publishes tracks before paced playback starts") {
  auto old_file_repeat =
      toolkit::mINI::Instance()[mediakit::Record::kFileRepeat];
  toolkit::onceToken restore_file_repeat(nullptr, [old_file_repeat]() {
    toolkit::mINI::Instance()[mediakit::Record::kFileRepeat] = old_file_repeat;
  });
  toolkit::mINI::Instance()[mediakit::Record::kFileRepeat] = true;

  MediaPlayer player;
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t track_count = 0;
  std::atomic_size_t frame_count = 0;
  std::atomic_bool play_result_called = false;
  std::atomic_bool shutdown_called = false;
  std::atomic_bool seeking = false;
  std::atomic_bool invalid_seek_frame = false;
  std::atomic_uint32_t seek_dts = UINT32_MAX;
  const auto control_thread = std::this_thread::get_id();
  std::atomic<ErrCode> play_result = Err_other;
  std::atomic<ErrCode> shutdown_result = Err_other;

  player.setOnPlayResult([&](const SockException &ex) {
    play_result = ex.getErrCode();
    auto tracks = player.getTracks(false);
    track_count = tracks.size();
    for (auto &track : tracks) {
      track->addDelegate([&](const mediakit::Frame::Ptr &frame) {
        if (seeking && std::this_thread::get_id() == control_thread) {
          auto expected = UINT32_MAX;
          if (!seek_dts.compare_exchange_strong(expected, frame->dts()) &&
              seek_dts != frame->dts()) {
            invalid_seek_frame = true;
          }
        }
        ++frame_count;
        return true;
      });
    }
    player.pause(true);
    play_result_called = true;
    condition.notify_all();
  });
  player.setOnShutdown([&](const SockException &ex) {
    shutdown_result = ex.getErrCode();
    shutdown_called = true;
    condition.notify_all();
  });

  player.play(samplePath());

  REQUIRE(play_result_called);
  REQUIRE(play_result == Err_success);
  REQUIRE(track_count == 1);
  CHECK(player.getDuration() == Catch::Approx(1.0f));

  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_for(lock, 700ms, [&]() { return shutdown_called.load(); });
  }
  CHECK(frame_count == 0);
  CHECK_FALSE(shutdown_called);

  seeking = true;
  player.seekTo(0.0f);
  seeking = false;
  CHECK_FALSE(invalid_seek_frame);
  player.speed(20.0f);

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(
        condition.wait_for(lock, 3s, [&]() { return shutdown_called.load(); }));
  }

  CHECK(shutdown_result == Err_eof);
  CHECK(frame_count >= 1);
  CHECK(player.getProgress() == Catch::Approx(1.0f));
}

TEST_CASE("file player control interrupts a slow 20x batch between frames") {
  MediaPlayer player;
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t frame_count = 0;
  std::atomic_bool frame_started = false;
  std::atomic_uint32_t last_dts = 0;

  player.setOnPlayResult([&](const SockException &ex) {
    if (ex) {
      return;
    }
    for (auto &track : player.getTracks(false)) {
      track->addDelegate([&](const mediakit::Frame::Ptr &frame) {
        ++frame_count;
        last_dts = frame->dts();
        frame_started = true;
        condition.notify_all();
        std::this_thread::sleep_for(100ms);
        return true;
      });
    }
    player.speed(20.0f);
  });

  player.play(samplePath());
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(
        condition.wait_for(lock, 2s, [&]() { return frame_started.load(); }));
  }

  auto begin = std::chrono::steady_clock::now();
  player.teardown();
  auto elapsed = std::chrono::steady_clock::now() - begin;

  CHECK(elapsed < 500ms);
  std::this_thread::sleep_for(700ms);
  CHECK(frame_count >= 1);
  CHECK(frame_count <= 3);
  CHECK(last_dts == 0);
}

TEST_CASE("missing local MP4 reports a play error without throwing") {
  MediaPlayer player;
  std::atomic_bool callback_called = false;
  std::atomic<ErrCode> result = Err_success;

  player.setOnPlayResult([&](const SockException &ex) {
    result = ex.getErrCode();
    callback_called = true;
  });

  CHECK_NOTHROW(player.play(samplePath() + ".missing"));
  REQUIRE(callback_called);
  CHECK(result == Err_other);
  CHECK(player.isFinite());
}

TEST_CASE("file proxy does not retry a failed finite input") {
  auto proxy = std::make_shared<PlayerProxy>(
      mediakit::MediaTuple("__defaultVhost__", "test", "file"),
      mediakit::ProtocolOption(), -1);
  std::atomic_size_t close_count = 0;
  std::atomic_size_t disconnect_count = 0;

  proxy->setOnClose([&](const SockException &) { ++close_count; });
  proxy->setOnDisconnect([&]() { ++disconnect_count; });

  proxy->play(samplePath() + ".missing");

  CHECK(close_count == 1);
  CHECK(disconnect_count == 0);
  CHECK(proxy->getRePullCount() == 0);
}

TEST_CASE("file proxy closes once at EOF without repulling") {
  auto proxy = std::make_shared<PlayerProxy>(
      mediakit::MediaTuple("__defaultVhost__", "test", "file-eof"),
      mediakit::ProtocolOption(), -1);
  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_size_t close_count = 0;
  std::atomic<ErrCode> close_result = Err_other;

  proxy->setOnClose([&](const SockException &ex) {
    close_result = ex.getErrCode();
    ++close_count;
    condition.notify_all();
  });

  proxy->play(samplePath());
  proxy->speed(20.0f);

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(condition.wait_for(lock, 3s,
                               [&]() { return close_count.load() == 1; }));
  }

  CHECK(close_result == Err_eof);
  CHECK(close_count == 1);
  CHECK(proxy->getRePullCount() == 0);
}

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Extension/Frame.h"
#include "Poller/EventPoller.h"
#include "Record/MP4Demuxer.h"
#include "mw/streamer/converter/zlm_codec_parameters_converter.h"
#include "mw/streamer/converter/zlm_packet_converter.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/ffmpeg/stream_info.h"
#include "mw/streamer/output/remux_sink.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::PacketSinkState;
using mw::streamer::RemuxSink;
using mw::streamer::RemuxSinkConfig;
using mw::streamer::StreamEndReason;

struct Sample {
  std::vector<mw::streamer::StreamInfo> streams;
  std::vector<mw::streamer::Packet> packets;
};

Sample ReadSample() {
  mediakit::MP4Demuxer input;
  input.openMP4(std::string(MW_REMUX_SINK_TEST_DATA_DIR) + "/h264_aac.mp4");
  Sample sample;
  std::unordered_map<int, std::unique_ptr<mw::streamer::ZlmPacketConverter>>
      converters;
  for (const auto& track : input.getTracks(true)) {
    const auto index = static_cast<int>(sample.streams.size());
    mw::streamer::ZlmCodecParametersConverter parameters(track);
    sample.streams.push_back(
        {index, parameters.codec_parameters(), parameters.time_base()});
    auto converter =
        std::make_unique<mw::streamer::ZlmPacketConverter>(track, index);
    converter->SetOnPacket([&sample](const auto& packet) {
      sample.packets.push_back(packet);
      return true;
    });
    converters.emplace(track->getIndex(), std::move(converter));
  }
  bool eof = false;
  while (!eof) {
    bool key = false;
    int error = 0;
    auto frame = input.readFrame(key, eof, &error);
    REQUIRE(error == 0);
    if (!frame) {
      continue;
    }
    if (key && !frame->keyFrame()) {
      frame = std::make_shared<mediakit::FrameCacheAble>(frame, true);
    }
    REQUIRE(converters.at(frame->getIndex())->InputFrame(frame));
  }
  for (const auto& entry : converters) {
    REQUIRE(entry.second->Flush());
  }
  REQUIRE(sample.packets.size() > 3);
  return sample;
}

class TemporaryFile final {
 public:
  TemporaryFile()
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-remux-overflow-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".mp4")) {}
  ~TemporaryFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// The sink selects its own pool member. Pause every member to make saturation
// deterministic without exposing a test-only Poller injection API.
class PollerPause final {
 public:
  ~PollerPause() { Release(); }

  void Pause() {
    std::vector<toolkit::TaskExecutor::Ptr> executors;
    toolkit::EventPollerPool::Instance().for_each(
        [&executors](const toolkit::TaskExecutor::Ptr& executor) {
          executors.push_back(executor);
        });
    expected_ = executors.size();
    for (const auto& executor : executors) {
      executor->async(
          [state = state_]() {
            std::unique_lock<std::mutex> lock(state->mutex);
            ++state->entered;
            state->condition.notify_all();
            state->condition.wait(lock, [&state] { return state->released; });
          },
          false);
    }
  }

  bool WaitUntilPaused() {
    std::unique_lock<std::mutex> lock(state_->mutex);
    return expected_ != 0 && state_->condition.wait_for(lock, 10s, [this] {
      return state_->entered == expected_;
    });
  }

  void Release() {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->released = true;
    }
    state_->condition.notify_all();
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t entered = 0;
    bool released = false;
  };

  // Tasks retain only this state, so exception unwinding can release the gate
  // before every queued task has started or returned.
  const std::shared_ptr<State> state_ = std::make_shared<State>();
  std::size_t expected_ = 0;
};

}  // namespace

TEST_CASE("RemuxSink包队列满时淘汰最旧包并保留生命周期通知") {
  const auto sample = ReadSample();
  TemporaryFile target;
  RemuxSinkConfig config;
  config.target = target.path().string();
  config.packet_queue_capacity = 1;
  RemuxSink sink("sink", config);

  // Declared after the sink so failure unwinding releases the Poller before
  // the sink's destructor waits for its shutdown barrier.
  PollerPause pause;
  pause.Pause();
  const bool paused = pause.WaitUntilPaused();
  bool latest_packet_retained = false;
  bool remained_healthy = false;
  bool snapshot_did_not_process_queue = false;
  if (paused) {
    sink.OnStreamsReady({1, sample.streams});
    sink.OnPacket({1, sample.packets[0]});
    latest_packet_retained =
        sink.state() == PacketSinkState::kIdle && sink.queue_depth() == 2;
    // This read must not dispatch a synchronous query to the paused Poller,
    // and queued packets are not completed remux processing calls.
    const auto snapshot = sink.GetPerformance();
    snapshot_did_not_process_queue =
        snapshot.operations.size() == 1 &&
        snapshot.operations.front().input_count == 0 &&
        snapshot.operations.front().started_calls == 0 &&
        snapshot.operations.front().output_count == 0;
    sink.OnPacket({1, sample.packets[1]});
    sink.OnPacket({1, sample.packets[2]});
    latest_packet_retained &= sink.queue_depth() == 2;
    remained_healthy =
        sink.state() == PacketSinkState::kIdle && sink.error().empty();
  }
  pause.Release();

  REQUIRE(paused);
  CHECK(latest_packet_retained);
  CHECK(snapshot_did_not_process_queue);
  CHECK(remained_healthy);
  REQUIRE(WaitUntil([&] { return sink.queue_depth() == 0; }));
  for (std::size_t index = 3; index < sample.packets.size(); ++index) {
    sink.OnPacket({1, sample.packets[index]});
    REQUIRE(WaitUntil([&] { return sink.queue_depth() == 0; }));
  }
  sink.OnInputEnded({1, StreamEndReason::kEof});
  REQUIRE(WaitUntil([&] {
    return sink.state() == PacketSinkState::kEnded ||
           sink.state() == PacketSinkState::kFailed;
  }));
  INFO(sink.error());
  CHECK(sink.state() == PacketSinkState::kEnded);
  CHECK(sink.error().empty());
  sink.Stop();
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kStopped);
  CHECK(sink.queue_depth() == 0);
}

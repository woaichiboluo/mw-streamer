#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Poller/EventPoller.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/stream_info.h"
#include "mw/output/remux_sink.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::output::RemuxSink;
using mw::streamer::output::RemuxSinkConfig;
using mw::streamer::sink::PacketSinkState;

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

TEST_CASE("RemuxSink包队列满时同步失败且生命周期通知不占包配额") {
  RemuxSinkConfig config;
  // The gate prevents opening the output; saturation discards the open work.
  config.target = "unused-remux-queue-overflow.mp4";
  config.packet_queue_capacity = 1;
  RemuxSink sink("sink", config);

  mw::streamer::ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = {1, 1000};
  stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  stream.codec_parameters.get()->codec_id = AV_CODEC_ID_H264;
  stream.codec_parameters.get()->width = 64;
  stream.codec_parameters.get()->height = 64;
  mw::streamer::ffmpeg::Packet packet;
  packet->stream_index = 0;
  packet->pts = 0;
  packet->dts = 0;

  // Declared after the sink so failure unwinding releases the Poller before
  // the sink's destructor waits for its shutdown barrier.
  PollerPause pause;
  pause.Pause();
  const bool paused = pause.WaitUntilPaused();
  bool first_packet_accepted = false;
  bool failed_synchronously = false;
  bool snapshot_did_not_process_queue = false;
  if (paused) {
    sink.OnStreamsReady({1, {stream}});
    sink.OnPacket({1, packet});
    first_packet_accepted =
        sink.state() == PacketSinkState::kIdle && sink.queue_depth() == 2;
    // This read must not dispatch a synchronous query to the paused Poller,
    // and queued packets are not completed remux processing calls.
    const auto snapshot = sink.GetPerformance();
    snapshot_did_not_process_queue =
        snapshot.operations.size() == 1 &&
        snapshot.operations.front().input_count == 0 &&
        snapshot.operations.front().started_calls == 0 &&
        snapshot.operations.front().output_count == 0;
    sink.OnPacket({1, packet});
    failed_synchronously =
        sink.state() == PacketSinkState::kFailed && sink.queue_depth() == 0;
  }
  pause.Release();

  REQUIRE(paused);
  CHECK(first_packet_accepted);
  CHECK(snapshot_did_not_process_queue);
  CHECK(failed_synchronously);
  CHECK(sink.error().find("队列已满") != std::string::npos);
  sink.Stop();
  sink.Stop();
  CHECK(sink.state() == PacketSinkState::kFailed);
  CHECK(sink.queue_depth() == 0);
}

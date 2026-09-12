#include "mw/streamer/input/file_input.h"

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/ffmpeg/input_format_context.h"

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

namespace {

using namespace std::chrono_literals;
using mw::streamer::InputFormatContext;
using mw::streamer::Packet;
using mw::streamer::FileInput;
using mw::streamer::Input;
using mw::streamer::InputState;
using mw::streamer::InputStateChanged;
using mw::streamer::PacketReady;
using mw::streamer::StreamDeliveryMode;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;

std::string Fixture(const char* name) {
  return std::string(MW_FILE_INPUT_TEST_DATA_DIR) + "/" + name;
}

class Observer final : public Input::Observer {
 public:
  void OnStreamsReady(const StreamsReady& value) noexcept override {
    streams.push_back(value);
  }

  void OnPacket(const PacketReady& value) noexcept override {
    if (packets.empty() && gate.valid()) {
      entered.set_value();
      gate.wait();
    }
    if (streams.empty() || !ends.empty()) {
      invalid_order = true;
    }
    packets.push_back(value.packet.Ref());
    callback_thread = std::this_thread::get_id();
  }

  void OnTimelineReset(const TimelineReset&) noexcept override { ++resets; }
  void OnInputEnded(const StreamEnded& value) noexcept override {
    if (streams.empty()) {
      invalid_order = true;
    }
    ends.push_back(value);
  }

  void OnInputStateChanged(const InputStateChanged& value) noexcept override {
    if (input && input->state() != value.state) {
      invalid_order = true;
    }
    states.push_back(value);
    if (value.state == InputState::kEnded ||
        value.state == InputState::kFailed ||
        value.state == InputState::kStopped) {
      finished.set_value();
    }
  }

  const Input* input = nullptr;
  std::vector<StreamsReady> streams;
  std::vector<Packet> packets;
  std::vector<StreamEnded> ends;
  std::vector<InputStateChanged> states;
  std::promise<void> finished;
  std::promise<void> entered;
  std::shared_future<void> gate;
  std::thread::id callback_thread;
  bool invalid_order = false;
  int resets = 0;
};

std::vector<Packet> ReadReference(const std::string& path) {
  InputFormatContext context(path);
  context.FindStreamInfo();
  std::vector<Packet> result;
  Packet packet;
  while (context.ReadPacket(packet)) {
    packet->time_base = context->streams[packet->stream_index]->time_base;
    result.push_back(packet.Ref());
    packet.Unref();
  }
  return result;
}

void CheckPacket(const Packet& actual, const Packet& expected) {
  CHECK(actual->pts == expected->pts);
  CHECK(actual->dts == expected->dts);
  CHECK(actual->duration == expected->duration);
  CHECK(actual->stream_index == expected->stream_index);
  CHECK(actual->time_base.num == expected->time_base.num);
  CHECK(actual->time_base.den == expected->time_base.den);
  CHECK(actual->flags == expected->flags);
  REQUIRE(actual->size == expected->size);
  CHECK(std::memcmp(actual->data, expected->data, actual->size) == 0);
  REQUIRE(actual->side_data_elems == expected->side_data_elems);
  for (int index = 0; index < actual->side_data_elems; ++index) {
    const auto& left = actual->side_data[index];
    const auto& right = expected->side_data[index];
    CHECK(left.type == right.type);
    REQUIRE(left.size == right.size);
    CHECK(std::memcmp(left.data, right.data, left.size) == 0);
  }
}

TEST_CASE(
    "FileInput preserves all packets, signed timestamps and AAC side data",
    "[file-input]") {
  const char* fixture = "h264_aac.mp4";
  SECTION("H264") {}
  SECTION("HEVC with reordered pictures") { fixture = "h265_aac.mp4"; }
  const auto path = Fixture(fixture);
  const auto reference = ReadReference(path);
  Observer observer;
  FileInput input({path});
  observer.input = &input;
  auto finished = observer.finished.get_future();
  input.Start(observer);
  const auto completion = finished.wait_for(3s);
  input.Stop();
  REQUIRE(completion == std::future_status::ready);
  REQUIRE(observer.streams.size() == 1);
  CHECK(observer.streams.front().delivery_mode == StreamDeliveryMode::kOffline);
  REQUIRE(observer.streams.front().streams.size() == 2);
  REQUIRE(observer.packets.size() == reference.size());
  bool has_negative_timestamp = false;
  bool has_skip_samples = false;
  std::uint64_t bytes = 0;
  for (std::size_t index = 0; index < reference.size(); ++index) {
    const auto& packet = observer.packets[index];
    CheckPacket(packet, reference[index]);
    bytes += packet->size;
    has_negative_timestamp |= packet->pts < 0 || packet->dts < 0;
    for (int side = 0; side < packet->side_data_elems; ++side) {
      has_skip_samples |=
          packet->side_data[side].type == AV_PKT_DATA_SKIP_SAMPLES;
    }
  }
  CHECK(has_negative_timestamp);
  CHECK(has_skip_samples);
  CHECK_FALSE(observer.invalid_order);
  CHECK(observer.resets == 0);
  CHECK(observer.callback_thread != std::this_thread::get_id());
  REQUIRE(observer.ends.size() == 1);
  CHECK(observer.ends.front().reason == StreamEndReason::kEof);
  REQUIRE(observer.states.size() == 3);
  CHECK(observer.states[0].state == InputState::kConnecting);
  CHECK(observer.states[1].state == InputState::kReady);
  CHECK(observer.states[2].state == InputState::kEnded);
  CHECK_FALSE(observer.states[2].will_retry);
  const auto snapshot = input.GetPerformance();
  REQUIRE(snapshot.operations.size() == 1);
  CHECK(snapshot.operations.front().output_count == reference.size());
  CHECK(snapshot.operations.front().output_bytes == bytes);
  CHECK_THROWS_AS(input.Start(observer), std::logic_error);
  input.Stop();
}

TEST_CASE("FileInput reads an eight second file without playback pacing",
          "[file-input]") {
  Observer observer;
  FileInput input({Fixture("packet_queue_8s.mp4")});
  auto finished = observer.finished.get_future();
  input.Start(observer);
  const auto completion = finished.wait_for(2s);
  input.Stop();
  REQUIRE(completion == std::future_status::ready);
  REQUIRE(observer.ends.size() == 1);
  CHECK(observer.ends.front().reason == StreamEndReason::kEof);
  CHECK_FALSE(observer.packets.empty());
}

TEST_CASE("FileInput waits for synchronous delivery when stopping",
          "[file-input]") {
  Observer observer;
  std::promise<void> release;
  observer.gate = release.get_future().share();
  auto entered = observer.entered.get_future();
  FileInput input({Fixture("h264_aac.mp4")});
  input.Start(observer);
  const auto entered_status = entered.wait_for(3s);
  if (entered_status != std::future_status::ready) {
    release.set_value();
    input.Stop();
    FAIL("FileInput did not enter packet delivery");
  }
  auto stopping = std::async(std::launch::async, [&input] { input.Stop(); });
  const auto stop_status = stopping.wait_for(100ms);
  release.set_value();
  stopping.get();
  CHECK(stop_status == std::future_status::timeout);
  REQUIRE(observer.packets.size() == 1);
  REQUIRE(observer.ends.size() == 1);
  CHECK(observer.ends.front().reason == StreamEndReason::kStopped);
  CHECK(observer.states.back().state == InputState::kStopped);
  CHECK(input.state() == InputState::kStopped);
  CHECK_FALSE(observer.invalid_order);
  input.Stop();
}

TEST_CASE("FileInput reports open failure without a fictitious stream end",
          "[file-input]") {
  Observer observer;
  FileInput input({Fixture("does-not-exist.mp4")});
  observer.input = &input;
  auto finished = observer.finished.get_future();
  input.Start(observer);
  const auto completion = finished.wait_for(3s);
  input.Stop();
  REQUIRE(completion == std::future_status::ready);
  CHECK(observer.streams.empty());
  CHECK(observer.packets.empty());
  CHECK(observer.ends.empty());
  REQUIRE(observer.states.size() == 2);
  CHECK(observer.states.back().state == InputState::kFailed);
  CHECK_FALSE(observer.states.back().error.empty());
  CHECK_FALSE(observer.states.back().will_retry);
  CHECK_FALSE(observer.invalid_order);
}

TEST_CASE("FileInput rejects empty paths and never restarts after Stop",
          "[file-input]") {
  Observer observer;
  FileInput invalid({""});
  CHECK_THROWS_AS(invalid.Start(observer), std::invalid_argument);
  CHECK(invalid.state() == InputState::kIdle);
  FileInput input({Fixture("h264_aac.mp4")});
  input.Stop();
  input.Stop();
  CHECK(input.state() == InputState::kStopped);
  CHECK_THROWS_AS(input.Start(observer), std::logic_error);
  CHECK(observer.states.empty());
}

}  // namespace

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/streamer/sink/packet_custom_sink_node.h"

namespace {

using namespace mw::streamer;
using namespace std::chrono_literals;

StreamsReady Streams(std::uint64_t generation = 1, int width = 64) {
  StreamInfo video;
  video.stream_index = 3;
  video.time_base = {1, 90000};
  auto* parameters = video.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->width = width;
  parameters->height = 32;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->framerate = {25, 1};

  StreamInfo audio;
  audio.stream_index = 7;
  audio.time_base = {1, 44100};
  parameters = audio.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->sample_rate = 44100;
  av_channel_layout_default(&parameters->ch_layout, 2);
  return {generation, {std::move(video), std::move(audio)}};
}

PacketReady PacketEvent(int index, std::uint64_t generation = 1) {
  Packet packet;
  REQUIRE(av_new_packet(packet.get(), 4) == 0);
  packet->stream_index = index;
  packet->pts = 123;
  packet->data[0] = 42;
  return {generation, std::move(packet)};
}

struct CallbackState {
  int starts = 0;
  int stops = 0;
  MwStreamerProcessorSourceInfo source{};
  const AVPacket* video = nullptr;
  const AVPacket* audio = nullptr;
  std::vector<MwStreamerProcessorBoundaryReason> boundaries;
};

MwStreamerPacketCustomSinkCallbacks Callbacks(CallbackState& state) {
  MwStreamerPacketCustomSinkCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerProcessorSourceInfo* source,
                          void* context) {
    auto& state = *static_cast<CallbackState*>(context);
    ++state.starts;
    state.source = *source;
    return kMwStreamerProcessorStartSuccess;
  };
  callbacks.on_video_packet = [](const void* packet, void* context) {
    static_cast<CallbackState*>(context)->video =
        static_cast<const AVPacket*>(packet);
  };
  callbacks.on_audio_packet = [](const void* packet, void* context) {
    static_cast<CallbackState*>(context)->audio =
        static_cast<const AVPacket*>(packet);
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* context) {
    static_cast<CallbackState*>(context)->boundaries.push_back(reason);
  };
  callbacks.on_stop = [](void* context) {
    ++static_cast<CallbackState*>(context)->stops;
  };
  return callbacks;
}

TEST_CASE("PacketCustomSink按轨道同步借用原始音视频包", "[custom-sink]") {
  CallbackState state;
  PacketCustomSink sink("packets", Callbacks(state));
  CHECK(sink.input_type() == SinkMediaType::kPacket);
  CHECK(sink.output_type() == SinkMediaType::kNone);
  sink.OnStreamsReady(Streams());
  const auto video = PacketEvent(3);
  const auto audio = PacketEvent(7);
  sink.OnPacket(video);
  sink.OnPacket(audio);
  CHECK(state.starts == 1);
  CHECK(state.source.has_video == 1);
  CHECK(state.source.has_audio == 1);
  CHECK(state.source.video.width == 64);
  CHECK(state.source.audio.sample_rate == 44100);
  CHECK(state.video == video.packet.get());
  CHECK(state.audio == audio.packet.get());
  REQUIRE(state.video);
  CHECK(state.video->data[0] == 42);
  CHECK(state.video->pts == 123);
}

TEST_CASE("PacketCustomSink单轨输入不会误分发无效轨道包", "[custom-sink]") {
  for (const bool audio_only : {false, true}) {
    for (const int index : {-1, 99}) {
      CAPTURE(audio_only, index);
      CallbackState state;
      std::string error;
      PacketCustomSink sink("packets", Callbacks(state));
      sink.SetOnFatalError([&](const std::string& value) { error = value; });
      auto streams = Streams();
      streams.streams.erase(streams.streams.begin() + (audio_only ? 0 : 1));
      sink.OnStreamsReady(streams);
      CHECK_NOTHROW(sink.OnPacket(PacketEvent(index)));
      CHECK_FALSE(error.empty());
      CHECK(state.video == nullptr);
      CHECK(state.audio == nullptr);
    }
  }
}

TEST_CASE("PacketCustomSink回调可克隆Packet并在原包释放后读取",
          "[custom-sink]") {
  Packet retained;
  MwStreamerPacketCustomSinkCallbacks callbacks{};
  callbacks.user_context = &retained;
  callbacks.on_video_packet = [](const void* packet, void* context) {
    *static_cast<Packet*>(context) =
        Packet(av_packet_clone(static_cast<const AVPacket*>(packet)));
  };
  PacketCustomSink sink("packets", callbacks);
  sink.OnStreamsReady(Streams());
  auto source = PacketEvent(3);
  sink.OnPacket(source);
  REQUIRE(retained.get());
  CHECK(retained.get() != source.packet.get());
  CHECK(retained->data == source.packet->data);
  source.packet.Unref();
  REQUIRE(retained->size == 4);
  CHECK(retained->data[0] == 42);
  CHECK(retained->pts == 123);
  CHECK(retained->stream_index == 3);
}

TEST_CASE("PacketCustomSink停止等待在途媒体回调后调用on_stop",
          "[custom-sink]") {
  struct State {
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> resume = release.get_future().share();
    std::atomic<bool> active{false};
    std::atomic<bool> stop_overlap{false};
    std::atomic<int> stops{0};
  } state;
  MwStreamerPacketCustomSinkCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_video_packet = [](const void*, void* context) {
    auto& state = *static_cast<State*>(context);
    state.active.store(true);
    state.entered.set_value();
    state.resume.wait();
    state.active.store(false);
  };
  callbacks.on_stop = [](void* context) {
    auto& state = *static_cast<State*>(context);
    state.stop_overlap.store(state.active.load());
    ++state.stops;
  };
  PacketCustomSink sink("packets", callbacks);
  sink.OnStreamsReady(Streams());
  const auto packet = PacketEvent(3);
  auto entered = state.entered.get_future();
  std::promise<void> stop_entered;
  auto stopping = stop_entered.get_future();
  auto media = std::async(std::launch::async, [&]() { sink.OnPacket(packet); });
  std::future<void> stop;
  // Release a blocked callback before either future joins during unwinding.
  struct ReleaseGuard {
    std::promise<void>& promise;
    bool released = false;
    void Release() {
      if (!released) {
        promise.set_value();
        released = true;
      }
    }
    ~ReleaseGuard() { Release(); }
  } guard{state.release};
  REQUIRE(entered.wait_for(2s) == std::future_status::ready);
  stop = std::async(std::launch::async, [&]() {
    stop_entered.set_value();
    sink.Stop();
  });
  REQUIRE(stopping.wait_for(2s) == std::future_status::ready);
  CHECK(stop.wait_for(50ms) == std::future_status::timeout);
  CHECK(state.stops.load() == 0);
  guard.Release();
  REQUIRE(media.wait_for(2s) == std::future_status::ready);
  REQUIRE(stop.wait_for(2s) == std::future_status::ready);
  CHECK_NOTHROW(media.get());
  CHECK_NOTHROW(stop.get());
  CHECK_FALSE(state.stop_overlap.load());
  CHECK(state.stops.load() == 1);
}

TEST_CASE("PacketCustomSink EOF与重置只通知边界且停止一次", "[custom-sink]") {
  CallbackState state;
  {
    PacketCustomSink sink("packets", Callbacks(state));
    sink.OnStreamsReady(Streams());
    sink.OnInputEnded({1, StreamEndReason::kEof});
    sink.OnInputEnded({1, StreamEndReason::kEof});
    CHECK(state.stops == 0);
    sink.OnTimelineReset({2, TimelineResetReason::kSeek});
    sink.OnTimelineReset({2, TimelineResetReason::kSeek});
    sink.OnStreamsReady(Streams(2));
    const auto video = PacketEvent(3, 2);
    sink.OnPacket(video);
    CHECK(state.video == video.packet.get());
    CHECK(state.starts == 1);
    CHECK(state.boundaries == std::vector<MwStreamerProcessorBoundaryReason>{
                                  kMwStreamerProcessorEndOfInput,
                                  kMwStreamerProcessorTimelineReset});
    sink.Stop();
    sink.Stop();
  }
  CHECK(state.stops == 1);
}

TEST_CASE("PacketCustomSink启动失败上报错误但不调用停止回调", "[custom-sink]") {
  CallbackState state;
  std::string error;
  auto callbacks = Callbacks(state);
  callbacks.on_start = [](const MwStreamerProcessorSourceInfo*, void*) {
    return kMwStreamerProcessorStartFailed;
  };
  {
    PacketCustomSink sink("packets", callbacks);
    sink.SetOnFatalError([&](const std::string& value) { error = value; });
    CHECK_NOTHROW(sink.OnStreamsReady(Streams()));
    CHECK_FALSE(error.empty());
    sink.Stop();
  }
  CHECK(state.stops == 0);
}

TEST_CASE("PacketCustomSink包含媒体回调异常并上报错误", "[custom-sink]") {
  CallbackState state;
  std::string error;
  auto callbacks = Callbacks(state);
  callbacks.on_video_packet = [](const void*, void*) {
    throw std::runtime_error("packet callback failed");
  };
  PacketCustomSink sink("packets", callbacks);
  sink.SetOnFatalError([&](const std::string& value) { error = value; });
  sink.OnStreamsReady(Streams());
  CHECK_NOTHROW(sink.OnPacket(PacketEvent(3)));
  CHECK(error.find("packet callback failed") != std::string::npos);
  sink.Stop();
  CHECK(state.stops == 1);
}

TEST_CASE("PacketCustomSink拒绝跨代次改变源信息", "[custom-sink]") {
  CallbackState state;
  std::string error;
  PacketCustomSink sink("packets", Callbacks(state));
  sink.SetOnFatalError([&](const std::string& value) { error = value; });
  sink.OnStreamsReady(Streams());
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect});
  CHECK_NOTHROW(sink.OnStreamsReady(Streams(2, 128)));
  CHECK_FALSE(error.empty());
  CHECK(state.starts == 1);
}

}  // namespace

#include "mw/encoder/encoder_sink.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/decoder/audio_decoder.h"
#include "mw/decoder/video_decoder.h"
#include "mw/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::EncoderSink;
using mw::streamer::EncoderSinkConfig;
using mw::streamer::EncoderSinkState;
using mw::streamer::Frame;
using mw::streamer::StreamInfo;
using mw::streamer::ThrowIfError;
using mw::streamer::FrameStreamsReady;
using mw::streamer::PacketReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;
using mw::streamer::TimelineResetReason;
using mw::streamer::PerformanceType;
using mw::streamer::PerformanceUnit;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;

EncoderSinkConfig SoftwareConfig(bool delayed = false) {
  EncoderSinkConfig config;
  config.audio_encoder.encoder_name = "aac";
  config.video_encoder.encoder_name = "libx264";
  config.video_encoder.frame_rate = {25, 1};
  config.video_encoder.properties = {{"preset", "ultrafast"}, {"threads", "1"}};
  if (delayed) {
    config.video_encoder.properties["preset"] = "medium";
    config.video_encoder.properties["rc-lookahead"] = "20";
    config.video_encoder.properties["sync-lookahead"] = "10";
  } else {
    config.video_encoder.properties["tune"] = "zerolatency";
  }
  return config;
}

FrameStreamsReady Streams(bool video, bool audio,
                          std::uint64_t generation = 1) {
  FrameStreamsReady ready{generation, {}, nullptr};
  if (video) {
    StreamInfo stream;
    stream.stream_index = 7;
    stream.time_base = {1, 90000};
    stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_H264;
    // Processor output can differ from original compressed-source metadata.
    stream.codec_parameters.get()->width = 1920;
    stream.codec_parameters.get()->height = 1080;
    stream.codec_parameters.get()->framerate = {25, 1};
    ready.source_streams.push_back(std::move(stream));
  }
  if (audio) {
    StreamInfo stream;
    stream.stream_index = 9;
    stream.time_base = {1, 44100};
    stream.codec_parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_AAC;
    stream.codec_parameters.get()->sample_rate = 44100;
    av_channel_layout_default(&stream.codec_parameters.get()->ch_layout, 2);
    ready.source_streams.push_back(std::move(stream));
  }
  return ready;
}

Frame Video(std::int64_t pts) {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 64;
  frame->height = 64;
  frame->time_base = {1, 25};
  frame->pts = pts;
  frame->duration = 1;
  frame->sample_aspect_ratio = {1, 1};
  ThrowIfError(av_frame_get_buffer(frame.get(), 32), "allocate test video");
  for (int plane = 0; plane < 3; ++plane) {
    std::memset(frame->data[plane], plane == 0 ? 16 + pts % 100 : 128,
                static_cast<std::size_t>(frame->linesize[plane]) *
                    (plane == 0 ? 64 : 32));
  }
  return frame;
}

Frame Audio(std::int64_t pts, int samples = 512) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->time_base = {1, 48000};
  frame->pts = pts;
  frame->duration = samples;
  av_channel_layout_default(&frame->ch_layout, 1);
  ThrowIfError(av_frame_get_buffer(frame.get(), 0), "allocate test audio");
  auto* data = reinterpret_cast<float*>(frame->data[0]);
  for (int index = 0; index < samples; ++index) {
    data[index] = static_cast<float>(
        0.2 * std::sin(2.0 * 3.141592653589793 * 440 * (pts + index) / 48000));
  }
  return frame;
}

bool WaitState(const EncoderSink& sink, EncoderSinkState expected) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (sink.state() != expected &&
         std::chrono::steady_clock::now() < deadline) {
    if (sink.state() == EncoderSinkState::kFailed) {
      return expected == EncoderSinkState::kFailed;
    }
    std::this_thread::yield();
  }
  return sink.state() == expected;
}

struct Recording {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<StreamsReady> streams;
  std::vector<PacketReady> packets;
  std::vector<StreamEnded> ends;
  std::vector<std::pair<std::string, std::uint64_t>> events;
  std::atomic<int> active{0};
  std::atomic<bool> serialized{true};
  bool block_first_packet = false;
  bool entered = false;
  bool released = false;
  int stops = 0;

  bool WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return entered; });
  }
  bool WaitEnds(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return ends.size() >= count; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};

// Declare after EncoderSink so a failed assertion cannot strand its worker.
struct ReleaseOnExit {
  Recording& recording;
  ~ReleaseOnExit() { recording.Release(); }
};

class PacketRecorder final : public Sink {
 public:
  explicit PacketRecorder(std::string id, Recording& recording)
      : Sink(std::move(id), SinkMediaType::kPacket), recording_(recording) {}

  void OnStreamsReady(const StreamsReady& ready) noexcept override {
    Enter();
    std::lock_guard<std::mutex> lock(recording_.mutex);
    recording_.streams.push_back(ready);
    recording_.events.emplace_back("ready", ready.generation);
    Leave();
  }
  void OnPacket(const PacketReady& ready) noexcept override {
    Enter();
    std::unique_lock<std::mutex> lock(recording_.mutex);
    recording_.packets.push_back(ready);
    recording_.events.emplace_back("packet", ready.generation);
    if (recording_.block_first_packet && !recording_.entered) {
      recording_.entered = true;
      recording_.changed.notify_all();
      recording_.changed.wait(lock, [&] { return recording_.released; });
    }
    Leave();
  }
  void OnTimelineReset(const TimelineReset& reset) noexcept override {
    Enter();
    std::lock_guard<std::mutex> lock(recording_.mutex);
    recording_.events.emplace_back("reset", reset.generation);
    Leave();
  }
  void OnInputEnded(const StreamEnded& end) noexcept override {
    Enter();
    std::lock_guard<std::mutex> lock(recording_.mutex);
    recording_.ends.push_back(end);
    recording_.events.emplace_back("end", end.generation);
    recording_.changed.notify_all();
    Leave();
  }
  void Stop() noexcept override {
    Enter();
    std::lock_guard<std::mutex> lock(recording_.mutex);
    ++recording_.stops;
    Leave();
  }
  PacketSinkState state() const noexcept { return PacketSinkState::kRunning; }

 private:
  void Enter() {
    if (recording_.active.fetch_add(1) != 0) {
      recording_.serialized.store(false);
    }
  }
  void Leave() { recording_.active.fetch_sub(1); }
  Recording& recording_;
};

std::size_t DecodeVideo(const Recording& recording,
                        std::uint64_t generation = 1) {
  using namespace mw::streamer;
  VideoDecoderConfig config;
  config.backend = VideoDecoderBackend::kSoftware;
  std::size_t count = 0;
  for (const auto& ready : recording.streams) {
    if (ready.generation != generation) {
      continue;
    }
    for (const auto& stream : ready.streams) {
      if (stream.codec_parameters.get()->codec_type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }
      CHECK(stream.codec_parameters.get()->width == 64);
      CHECK(stream.codec_parameters.get()->height == 64);
      VideoDecoder decoder(stream, config);
      decoder.SetOnFrame([&](const Frame& frame) {
        CHECK(frame->width == 64);
        CHECK(frame->height == 64);
        ++count;
      });
      for (const auto& packet : recording.packets) {
        if (packet.generation == generation &&
            packet.packet->stream_index == stream.stream_index) {
          decoder.Decode(packet.packet);
        }
      }
      decoder.Drain();
    }
  }
  return count;
}

std::size_t DecodeAudio(const Recording& recording) {
  std::size_t samples = 0;
  for (const auto& ready : recording.streams) {
    for (const auto& stream : ready.streams) {
      if (stream.codec_parameters.get()->codec_type != AVMEDIA_TYPE_AUDIO) {
        continue;
      }
      CHECK(stream.codec_parameters.get()->sample_rate == 48000);
      CHECK(stream.codec_parameters.get()->ch_layout.nb_channels == 1);
      mw::streamer::AudioDecoder decoder(stream);
      decoder.SetOnFrame([&](const Frame& frame) {
        CHECK(frame->sample_rate == 48000);
        samples += frame->nb_samples;
      });
      for (const auto& packet : recording.packets) {
        if (packet.packet->stream_index == stream.stream_index) {
          decoder.Decode(packet.packet);
        }
      }
      decoder.Drain();
    }
  }
  return samples;
}

void CheckOrdered(const Recording& recording) {
  std::uint64_t generation = 0;
  bool ready = false;
  for (const auto& event : recording.events) {
    if (event.first == "reset") {
      generation = event.second;
      ready = false;
    } else if (event.first == "ready") {
      generation = event.second;
      ready = true;
    } else if (event.first == "packet") {
      CHECK(ready);
      CHECK(event.second == generation);
    } else if (event.first == "end") {
      ready = false;
    }
  }
  CHECK(recording.serialized.load());
}

}  // namespace

TEST_CASE("encoder sink fans out decodable delayed video and drains EOF") {
  Recording first;
  Recording second;
  EncoderSink sink("sink", SoftwareConfig(true));
  sink.AddSink(std::make_unique<PacketRecorder>("first", first));
  sink.AddSink(std::make_unique<PacketRecorder>("second", second));
  sink.OnStreamsReady(Streams(true, false));
  for (int index = 0; index < 12; ++index) {
    sink.OnVideoFrame({1, Video(index)});
  }
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  REQUIRE(first.streams.size() == 1);
  REQUIRE(first.streams.front().streams.size() == 1);
  REQUIRE(first.packets.size() == 12);
  REQUIRE(second.packets.size() == first.packets.size());
  CHECK(DecodeVideo(first) == 12);
  const auto snapshot = sink.GetPerformance();
  CHECK(snapshot.name == "EncoderSink");
  REQUIRE(snapshot.downstream.size() == 2);
  REQUIRE(snapshot.operations.size() == 2);
  const auto& video = snapshot.operations.at(1);
  CHECK(video.type == PerformanceType::kVideoEncoder);
  CHECK(video.input_unit == PerformanceUnit::kFrame);
  CHECK(video.output_unit == PerformanceUnit::kPacket);
  CHECK(video.input_count == 12);
  CHECK(video.output_count == 12);
  CHECK(video.started_calls == 13);
  CHECK(video.completed_calls == 13);
  CHECK(video.in_flight == 0);
  CHECK(video.failed_calls == 0);
  std::uint64_t bytes = 0;
  for (const auto& ready : first.packets) {
    bytes += ready.packet->size;
  }
  CHECK(video.output_bytes == bytes);
  CHECK(sink.GetPerformance().operations.at(1).output_count == 12);
  for (std::size_t index = 0; index < first.packets.size(); ++index) {
    const auto& left = first.packets[index].packet;
    const auto& right = second.packets[index].packet;
    REQUIRE(left->size > 0);
    CHECK(left->data == right->data);
    CHECK(left->pts == right->pts);
    CHECK(left->buf->buffer == right->buf->buffer);
  }
  REQUIRE(first.ends.size() == 1);
  CHECK(first.ends.front().reason == StreamEndReason::kEof);
  CHECK(first.stops == 1);
  CheckOrdered(first);
  CheckOrdered(second);
}

TEST_CASE("encoder sink drains a partial AAC FIFO for audio-only input") {
  Recording recording;
  EncoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(false, true));
  sink.OnAudioFrame({1, Audio(0, 512)});
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  REQUIRE(recording.streams.size() == 1);
  REQUIRE(recording.streams.front().streams.size() == 1);
  CHECK_FALSE(recording.packets.empty());
  CHECK(DecodeAudio(recording) >= 512);
  const auto snapshot = sink.GetPerformance();
  const auto& audio = snapshot.operations.at(0);
  CHECK(audio.type == PerformanceType::kAudioEncoder);
  CHECK(audio.input_unit == PerformanceUnit::kSample);
  CHECK(audio.input_count == 512);
  CHECK(audio.output_count == recording.packets.size());
  CHECK(audio.completed_calls == 2);
  CHECK(audio.failed_calls == 0);
  CheckOrdered(recording);
}

TEST_CASE("encoder sink serializes concurrent audio and video producers") {
  Recording recording;
  EncoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, true));
  std::thread audio([&] {
    for (int index = 0; index < 30; ++index) {
      sink.OnAudioFrame({1, Audio(index * 512)});
    }
  });
  std::thread video([&] {
    for (int index = 0; index < 30; ++index) {
      sink.OnVideoFrame({1, Video(index)});
    }
  });
  audio.join();
  video.join();
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  REQUIRE(recording.streams.size() == 1);
  REQUIRE(recording.streams.front().streams.size() == 2);
  CHECK(DecodeVideo(recording) == 30);
  CHECK(DecodeAudio(recording) >= 30 * 512);
  CheckOrdered(recording);
}

TEST_CASE("encoder sink validates registration and positive queue bounds") {
  CHECK_THROWS_AS(EncoderSink("encoder-1",
                              [] {
                                auto config = SoftwareConfig();
                                config.frame_queue_capacity = 0;
                                return config;
                              }()),
                  std::invalid_argument);
  CHECK_THROWS_AS(EncoderSink("encoder-2",
                              [] {
                                auto config = SoftwareConfig();
                                config.startup_packet_capacity = 0;
                                return config;
                              }()),
                  std::invalid_argument);
  Recording recording;
  EncoderSink sink("sink", SoftwareConfig());
  CHECK_THROWS_AS(sink.AddSink(nullptr), std::invalid_argument);
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  CHECK_THROWS_AS(
      sink.AddSink(std::make_unique<PacketRecorder>("recording", recording)),
      std::logic_error);
  sink.Stop();
  sink.Stop();
  CHECK(recording.stops == 1);
}

TEST_CASE("encoder sink bounds packets waiting for an unopened track") {
  Recording recording;
  auto config = SoftwareConfig();
  config.startup_packet_capacity = 1;
  EncoderSink sink("sink", config);
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, true));
  for (int index = 0; index < 5; ++index) {
    sink.OnVideoFrame({1, Video(index)});
  }
  const bool failed = WaitState(sink, EncoderSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording.streams.empty());
  CHECK(recording.packets.empty());
  CHECK(recording.stops == 1);
}

TEST_CASE("encoder sink bounds frames while a downstream call is blocked") {
  Recording recording;
  recording.block_first_packet = true;
  auto config = SoftwareConfig();
  config.frame_queue_capacity = 1;
  EncoderSink sink("sink", config);
  ReleaseOnExit release{recording};
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recording.WaitEntered();
  if (entered) {
    sink.OnVideoFrame({1, Video(1)});
    CHECK(sink.queue_depth() <= 1);
    sink.OnVideoFrame({1, Video(2)});
  }
  recording.Release();
  const bool failed = WaitState(sink, EncoderSinkState::kFailed);
  sink.Stop();
  REQUIRE(entered);
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording.stops == 1);
  REQUIRE(recording.ends.size() == 1);
  CHECK(recording.ends.front().reason == StreamEndReason::kFailed);
}

TEST_CASE(
    "encoder sink reset discards pending old frames and restarts codecs") {
  Recording recording;
  recording.block_first_packet = true;
  EncoderSink sink("sink", SoftwareConfig());
  ReleaseOnExit release{recording};
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recording.WaitEntered();
  if (entered) {
    for (int index = 1; index <= 5; ++index) {
      sink.OnVideoFrame({1, Video(index)});
    }
    sink.OnTimelineReset({2, TimelineResetReason::kSeek, 0ms});
    sink.OnStreamsReady(Streams(true, false, 2));
    sink.OnVideoFrame({2, Video(0)});
    sink.OnVideoFrame({2, Video(1)});
    sink.OnInputEnded({2, StreamEndReason::kEof});
  }
  recording.Release();
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(entered);
  REQUIRE(ended);
  std::size_t old_packets = 0;
  bool first_new = true;
  for (const auto& packet : recording.packets) {
    if (packet.generation == 1) {
      ++old_packets;
    } else if (first_new) {
      CHECK((packet.packet->flags & AV_PKT_FLAG_KEY) != 0);
      first_new = false;
    }
  }
  CHECK(old_packets == 1);
  CHECK_FALSE(first_new);
  CHECK(DecodeVideo(recording, 2) == 2);
  CheckOrdered(recording);
}

TEST_CASE("encoder sink interruption permits a new generation without EOF") {
  Recording recording;
  EncoderSink sink("sink", SoftwareConfig(true));
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  const bool interrupted = recording.WaitEnds(1);
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  sink.OnStreamsReady(Streams(true, false, 2));
  sink.OnVideoFrame({2, Video(0)});
  sink.OnInputEnded({2, StreamEndReason::kEof});
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(interrupted);
  REQUIRE(ended);
  REQUIRE(recording.ends.size() == 2);
  CHECK(recording.ends[0].reason == StreamEndReason::kInterrupted);
  CHECK(recording.ends[1].reason == StreamEndReason::kEof);
  for (const auto& packet : recording.packets) {
    CHECK(packet.generation == 2);
  }
  CHECK(DecodeVideo(recording, 2) == 1);
  CheckOrdered(recording);
}

TEST_CASE("encoder sink stop aborts queued frames without manufacturing EOF") {
  Recording recording;
  recording.block_first_packet = true;
  EncoderSink sink("sink", SoftwareConfig());
  ReleaseOnExit release{recording};
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recording.WaitEntered();
  if (entered) {
    for (int index = 1; index < 10; ++index) {
      sink.OnVideoFrame({1, Video(index)});
    }
  }
  std::thread stopper([&] { sink.Stop(); });
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (sink.queue_depth() != 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto pending = sink.queue_depth();
  recording.Release();
  stopper.join();
  REQUIRE(entered);
  CHECK(pending == 0);
  CHECK(recording.packets.size() == 1);
  CHECK(recording.ends.empty());
  CHECK(recording.stops == 1);
  CHECK(sink.state() == EncoderSinkState::kStopped);
}

TEST_CASE("encoder sink exposes asynchronous codec failures") {
  Recording recording;
  auto config = SoftwareConfig();
  config.video_encoder.encoder_name = "missing_encoder_for_test";
  EncoderSink sink("sink", config);
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  const bool failed = WaitState(sink, EncoderSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording.packets.empty());
  CHECK(recording.stops == 1);
}

TEST_CASE(
    "encoder sink rejects EOF when a declared track never received frames") {
  Recording recording;
  EncoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, true));
  sink.OnVideoFrame({1, Video(0)});
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const bool failed = WaitState(sink, EncoderSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording.ends.empty());
}

TEST_CASE("encoder sink refuses input without a packet consumer") {
  EncoderSink sink("sink", SoftwareConfig());
  sink.OnStreamsReady(Streams(true, false));
  const bool failed = WaitState(sink, EncoderSinkState::kFailed);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
}

TEST_CASE(
    "encoder sink forwards consecutive resets across a frameless generation") {
  Recording recording;
  recording.block_first_packet = true;
  EncoderSink sink("sink", SoftwareConfig());
  ReleaseOnExit release{recording};
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  const bool entered = recording.WaitEntered();
  if (entered) {
    sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
    sink.OnStreamsReady(Streams(true, false, 2));
    sink.OnTimelineReset({3, TimelineResetReason::kReconnect, std::nullopt});
    sink.OnStreamsReady(Streams(true, false, 3));
    sink.OnVideoFrame({3, Video(0)});
    sink.OnInputEnded({3, StreamEndReason::kEof});
  }
  recording.Release();
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(entered);
  REQUIRE(ended);
  const std::vector<std::pair<std::string, std::uint64_t>> expected{
      {"ready", 1}, {"packet", 1}, {"reset", 2}, {"reset", 3},
      {"ready", 3}, {"packet", 3}, {"end", 3}};
  CHECK(recording.events == expected);
  CHECK(DecodeVideo(recording, 3) == 1);
  CheckOrdered(recording);
}

TEST_CASE("encoder statistics measure work and exclude downstream blocking") {
  Recording recording;
  recording.block_first_packet = true;
  EncoderSink sink("sink", SoftwareConfig());
  ReleaseOnExit release{recording};
  sink.AddSink(std::make_unique<PacketRecorder>("recording", recording));
  sink.OnStreamsReady(Streams(true, false));
  sink.OnVideoFrame({1, Video(0)});
  REQUIRE(recording.WaitEntered());
  for (int index = 1; index < 5; ++index) {
    sink.OnVideoFrame({1, Video(index)});
  }
  sink.OnInputEnded({1, StreamEndReason::kEof});
  const auto during = sink.GetPerformance();
  const auto& active = during.operations.at(1);
  CHECK(active.in_flight == 1);
  CHECK(active.started_calls == 1);
  CHECK(active.input_count == 1);
  CHECK(active.output_count == 1);
  const auto blocked_at = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(300ms);
  const auto blocked_time = std::chrono::steady_clock::now() - blocked_at;
  recording.Release();
  const bool ended = WaitState(sink, EncoderSinkState::kEnded);
  sink.Stop();
  REQUIRE(ended);
  const auto after = sink.GetPerformance();
  const auto& video = after.operations.at(1);
  CHECK(video.input_count == 5);
  CHECK(video.output_count == 5);
  CHECK(video.completed_calls == 6);
  CHECK(video.in_flight == 0);
  CHECK(video.total_time < blocked_time);
}

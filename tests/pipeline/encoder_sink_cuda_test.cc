#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/decoder/video_decoder.h"
#include "mw/encoder/encoder_sink.h"
#include "mw/ffmpeg/error.h"
#include "mw/synchronizer/synchronizer_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::EncoderSink;
using mw::streamer::EncoderSinkConfig;
using mw::streamer::EncoderSinkState;
using mw::streamer::FrameStreamsReady;
using mw::streamer::PacketReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::StreamsReady;
using mw::streamer::TimelineReset;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using mw::streamer::SynchronizerSink;
using mw::streamer::SynchronizerSinkConfig;
using mw::streamer::SynchronizerSinkState;
using namespace mw::streamer;

constexpr int kWidth = 256;
constexpr int kHeight = 144;
constexpr std::int64_t kFrameCount = 8;
constexpr AVRational kTimeBase{1, 25};

Frame MakeCudaFrame(const HardwareContext& device) {
  AVBufferRef* pool =
      av_hwframe_ctx_alloc(const_cast<AVBufferRef*>(device.get()));
  if (!pool) {
    throw std::bad_alloc();
  }
  auto* context = reinterpret_cast<AVHWFramesContext*>(pool->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = kWidth;
  context->height = kHeight;
  context->initial_pool_size = 4;
  try {
    ThrowIfError(av_hwframe_ctx_init(pool), "初始化编码测试CUDA帧池");
    Frame software;
    software->format = AV_PIX_FMT_NV12;
    software->width = kWidth;
    software->height = kHeight;
    ThrowIfError(av_frame_get_buffer(software.get(), 32),
                         "分配编码测试上传帧");
    std::memset(software->data[0], 32,
                static_cast<std::size_t>(software->linesize[0]) * kHeight);
    std::memset(software->data[1], 128,
                static_cast<std::size_t>(software->linesize[1]) * kHeight / 2);
    Frame frame;
    ThrowIfError(av_hwframe_get_buffer(pool, frame.get(), 0),
                         "分配编码测试CUDA帧");
    ThrowIfError(
        av_hwframe_transfer_data(frame.get(), software.get(), 0),
        "上传编码测试CUDA帧");
    frame->time_base = kTimeBase;
    frame->pts = 0;
    frame->duration = 1;
    frame->sample_aspect_ratio = {1, 1};
    frame->color_range = AVCOL_RANGE_MPEG;
    av_buffer_unref(&pool);
    return frame;
  } catch (...) {
    av_buffer_unref(&pool);
    throw;
  }
}

class DeliveryGate final {
 public:
  void Enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    timed_out_ = !cv_.wait_for(lock, 10s, [this] { return released_; });
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 10s, [this] { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  bool timed_out() const { return timed_out_; }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  bool timed_out_ = false;
};

struct Recording {
  std::vector<StreamsReady> sources;
  std::vector<PacketReady> packets;
  std::vector<StreamEnded> ends;
  std::size_t packet_count_at_end = 0;
  int resets = 0;
  int stops = 0;
};

class PacketRecorder final : public Sink {
 public:
  explicit PacketRecorder(std::string id, Recording& recording,
                          DeliveryGate* gate = nullptr)
      : Sink(std::move(id), SinkMediaType::kPacket),
        recording_(recording),
        gate_(gate) {}

  void OnStreamsReady(const StreamsReady& streams) noexcept override {
    recording_.sources.push_back(streams);
    state_.store(PacketSinkState::kRunning);
    if (gate_) {
      gate_->Enter();
    }
  }

  void OnPacket(const PacketReady& packet) noexcept override {
    recording_.packets.push_back({packet.generation, packet.packet.Ref()});
    packet_count_.fetch_add(1);
  }

  void OnTimelineReset(const TimelineReset&) noexcept override {
    ++recording_.resets;
  }

  void OnInputEnded(const StreamEnded& end) noexcept override {
    recording_.packet_count_at_end = recording_.packets.size();
    recording_.ends.push_back(end);
    state_.store(PacketSinkState::kEnded);
  }

  void Stop() noexcept override {
    if (state_.exchange(PacketSinkState::kStopped) !=
        PacketSinkState::kStopped) {
      ++recording_.stops;
    }
  }

  PacketSinkState state() const noexcept { return state_.load(); }

  std::size_t packet_count() const noexcept { return packet_count_.load(); }

 private:
  Recording& recording_;
  DeliveryGate* gate_;
  std::atomic<PacketSinkState> state_{PacketSinkState::kIdle};
  std::atomic<std::size_t> packet_count_{0};
};

FrameStreamsReady MakeStreams(const HardwareContext& device) {
  StreamInfo stream;
  stream.stream_index = 3;
  stream.time_base = kTimeBase;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->width = kWidth;
  parameters->height = kHeight;
  parameters->format = AV_PIX_FMT_NV12;
  parameters->framerate = {25, 1};
  return {1, {std::move(stream)}, &device};
}

}  // namespace

TEST_CASE(
    "EncoderSink retains queued CUDA frames and fans out encoded packets") {
  EncoderSinkConfig config;
  AVCodecID expected_codec = AV_CODEC_ID_H264;
  SECTION("H264 NVENC") { config.video_encoder.codec = kMwStreamerCodecH264; }
  SECTION("H265 NVENC") {
    config.video_encoder.codec = kMwStreamerCodecH265;
    expected_codec = AV_CODEC_ID_HEVC;
  }
  config.video_encoder.frame_rate = {25, 1};
  config.video_encoder.properties = {{"preset", "p1"}, {"tune", "ull"}};

  const auto device = HardwareContext::CreateCuda(0);
  Recording first;
  Recording second;
  DeliveryGate gate;
  auto sink = std::make_unique<EncoderSink>("sink", config);
  sink->AddSink(std::make_unique<PacketRecorder>("first", first, &gate));
  sink->AddSink(std::make_unique<PacketRecorder>("second", second));
  sink->OnStreamsReady(MakeStreams(device));
  bool worker_entered = false;
  {
    auto prototype = MakeCudaFrame(device);
    sink->OnVideoFrame({1, prototype.Ref()});
    worker_entered = gate.Wait();
    for (std::int64_t pts = 1; pts < kFrameCount; ++pts) {
      auto frame = prototype.Ref();
      frame->pts = pts;
      sink->OnVideoFrame({1, std::move(frame)});
    }
    // All caller-owned AVFrames and the frame-pool handle disappear while the
    // worker is held at its first downstream notification. Queued references
    // must keep the CUDA allocations alive until encoding consumes them.
  }
  sink->OnInputEnded({1, StreamEndReason::kEof});
  gate.Release();
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (sink->state() != EncoderSinkState::kEnded &&
         sink->state() != EncoderSinkState::kFailed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  const auto state = sink->state();
  const auto error = sink->error();
  sink->Stop();
  sink.reset();

  INFO(error);
  REQUIRE(worker_entered);
  CHECK_FALSE(gate.timed_out());
  REQUIRE(state == EncoderSinkState::kEnded);
  for (const Recording* recording : {&first, &second}) {
    REQUIRE(recording->sources.size() == 1);
    CHECK(recording->sources.front().generation == 1);
    REQUIRE(recording->sources.front().streams.size() == 1);
    const auto& stream = recording->sources.front().streams.front();
    CHECK(stream.codec_parameters.get()->codec_id == expected_codec);
    REQUIRE(recording->packets.size() == kFrameCount);
    REQUIRE(recording->ends.size() == 1);
    CHECK(recording->ends.front().generation == 1);
    CHECK(recording->ends.front().reason == StreamEndReason::kEof);
    CHECK(recording->packet_count_at_end == kFrameCount);
    CHECK(recording->resets == 0);
    CHECK(recording->stops == 1);

    VideoDecoderConfig decoder_config;
    decoder_config.backend = VideoDecoderBackend::kSoftware;
    VideoDecoder decoder(stream, decoder_config);
    std::size_t decoded_frames = 0;
    decoder.SetOnFrame([&](const Frame& frame) {
      CHECK(frame->width == kWidth);
      CHECK(frame->height == kHeight);
      CHECK(frame->pts == static_cast<std::int64_t>(decoded_frames));
      ++decoded_frames;
    });
    for (const auto& packet : recording->packets) {
      CHECK(packet.generation == 1);
      CHECK(packet.packet->stream_index == stream.stream_index);
      decoder.Decode(packet.packet);
    }
    decoder.Drain();
    CHECK(decoded_frames == kFrameCount);
  }
  for (std::size_t i = 0; i < first.packets.size(); ++i) {
    REQUIRE(first.packets[i].packet->buf != nullptr);
    REQUIRE(second.packets[i].packet->buf != nullptr);
    CHECK(first.packets[i].packet->buf->buffer ==
          second.packets[i].packet->buf->buffer);
    CHECK(first.packets[i].packet->data == second.packets[i].packet->data);
  }
}

TEST_CASE("Synchronizer CUDA standby remains decodable through NVENC") {
  const auto device = HardwareContext::CreateCuda(0);
  Recording recording;
  auto packets = std::make_unique<PacketRecorder>("recording", recording);
  const auto* captured = packets.get();
  EncoderSinkConfig encoder_config;
  encoder_config.video_encoder.codec = kMwStreamerCodecH264;
  encoder_config.video_encoder.frame_rate = {25, 1};
  // Keep the natural GOP longer than the test so a second keyframe verifies
  // the synchronizer's standby transition request reaches NVENC.
  encoder_config.video_encoder.properties = {
      {"preset", "p1"}, {"tune", "ull"}, {"g", "250"}, {"bf", "0"}};
  auto encoder = std::make_unique<EncoderSink>("encoder", encoder_config);
  const auto* encoding = encoder.get();
  encoder->AddSink(std::move(packets));
  SynchronizerSinkConfig sync_config;
  sync_config.standby_timeout = 80ms;
  SynchronizerSink sink("sink", sync_config);
  sink.AddSink(std::move(encoder));
  sink.OnStreamsReady(MakeStreams(device));
  sink.OnVideoFrame({1, MakeCudaFrame(device)});

  const auto standby_deadline = std::chrono::steady_clock::now() + 5s;
  while ((sink.state() != SynchronizerSinkState::kStandby ||
          captured->packet_count() < 6) &&
         sink.state() != SynchronizerSinkState::kFailed &&
         encoding->state() != EncoderSinkState::kFailed &&
         std::chrono::steady_clock::now() < standby_deadline) {
    std::this_thread::sleep_for(5ms);
  }
  const bool standby_encoded =
      sink.state() == SynchronizerSinkState::kStandby &&
      captured->packet_count() >= 6;
  if (standby_encoded) {
    sink.OnInputEnded({1, StreamEndReason::kEof});
    const auto end_deadline = std::chrono::steady_clock::now() + 10s;
    while ((encoding->state() != EncoderSinkState::kEnded ||
            sink.state() != SynchronizerSinkState::kEnded) &&
           encoding->state() != EncoderSinkState::kFailed &&
           sink.state() != SynchronizerSinkState::kFailed &&
           std::chrono::steady_clock::now() < end_deadline) {
      std::this_thread::sleep_for(5ms);
    }
  }
  const auto sync_state = sink.state();
  const auto encoder_state = encoding->state();
  const auto sync_error = sink.error();
  const auto encoder_error = encoding->error();
  sink.Stop();

  INFO(sync_error);
  INFO(encoder_error);
  REQUIRE(standby_encoded);
  REQUIRE(sync_state == SynchronizerSinkState::kEnded);
  REQUIRE(encoder_state == EncoderSinkState::kEnded);
  REQUIRE(recording.sources.size() == 1);
  REQUIRE(recording.sources.front().streams.size() == 1);
  CHECK(recording.sources.front().generation == 1);
  REQUIRE(recording.ends.size() == 1);
  CHECK(recording.ends.front().generation == 1);
  CHECK(recording.ends.front().reason == StreamEndReason::kEof);
  CHECK(recording.packet_count_at_end == recording.packets.size());
  CHECK(recording.resets == 0);
  CHECK(recording.stops == 1);
  REQUIRE(recording.packets.size() >= 6);
  REQUIRE(recording.packets.front().packet->flags & AV_PKT_FLAG_KEY);

  const auto& stream = recording.sources.front().streams.front();
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264);
  VideoDecoderConfig decoder_config;
  decoder_config.backend = VideoDecoderBackend::kSoftware;
  VideoDecoder decoder(stream, decoder_config);
  std::size_t decoded_frames = 0;
  bool standby_image_decoded = false;
  decoder.SetOnFrame([&](const Frame& frame) {
    CHECK(frame->width == kWidth);
    CHECK(frame->height == kHeight);
    CHECK(av_compare_ts(frame->pts, stream.time_base,
                        static_cast<std::int64_t>(decoded_frames),
                        kTimeBase) == 0);
    if (decoded_frames == 0) {
      CHECK(frame->data[0][0] >= 30);
      CHECK(frame->data[0][0] <= 34);
    } else {
      for (int row = 0; row < frame->height && !standby_image_decoded; ++row) {
        for (int column = 0; column < frame->width; ++column) {
          const auto sample = frame->data[0][row * frame->linesize[0] + column];
          if (sample < 30 || sample > 34) {
            standby_image_decoded = true;
            break;
          }
        }
      }
    }
    ++decoded_frames;
  });
  int key_frames = 0;
  for (std::size_t index = 0; index < recording.packets.size(); ++index) {
    const auto& packet = recording.packets[index];
    CHECK(packet.generation == 1);
    CHECK(av_compare_ts(packet.packet->pts, stream.time_base,
                        static_cast<std::int64_t>(index), kTimeBase) == 0);
    key_frames += (packet.packet->flags & AV_PKT_FLAG_KEY) != 0;
    decoder.Decode(packet.packet);
  }
  decoder.Drain();
  CHECK(decoded_frames == recording.packets.size());
  CHECK(standby_image_decoded);
  CHECK(key_frames >= 2);
}

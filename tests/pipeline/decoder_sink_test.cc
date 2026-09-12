#include "mw/streamer/decoder/decoder_sink.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
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

#ifdef CHECK
#undef CHECK
#endif
#include <catch2/catch_test_macros.hpp>

#include "mw/streamer/ffmpeg/input_format_context.h"
#include "mw/streamer/input/zlm_input.h"
#include "mw/streamer/pipeline/pipeline.h"
#include "mw/streamer/processor/analysis_processor_sink.h"
#include "mw/streamer/processor/transform_processor_sink.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::DecoderSink;
using mw::streamer::DecoderSinkConfig;
using mw::streamer::ZlmInput;
using mw::streamer::ZlmInputConfig;
using mw::streamer::FrameReady;
using mw::streamer::FrameStreamsReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::TimelineReset;
using mw::streamer::TimelineResetReason;
using mw::streamer::AnalysisProcessorSink;
using mw::streamer::TransformProcessorSink;
using mw::streamer::FatalError;
using mw::streamer::PacketSinkState;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using namespace mw::streamer;
using mw::streamer::VideoDecoderBackend;
using mw::streamer::CodecParameters;
using mw::streamer::InputFormatContext;
using mw::streamer::Packet;
using mw::streamer::StreamInfo;
using mw::streamer::PerformanceType;
using mw::streamer::PerformanceUnit;

std::string SamplePath() {
  return std::string(MW_DECODER_SINK_TEST_DATA_DIR) + "/h264_aac.mp4";
}

DecoderSinkConfig SoftwareConfig(std::chrono::milliseconds cache = 0ms) {
  DecoderSinkConfig config;
  config.cache_duration = cache;
  config.video_decoder.backend = VideoDecoderBackend::kSoftware;
  return config;
}

std::uint64_t FrameHash(const FrameReady& ready, bool audio) {
  const auto& frame = ready.frame;
  const auto bytes =
      audio ? frame->nb_samples * frame->ch_layout.nb_channels * sizeof(float)
            : frame->width;
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t i = 0; i < bytes; ++i) {
    hash = (hash ^ frame->data[0][i]) * 1099511628211ULL;
  }
  return hash;
}

struct DeliveryOrder {
  std::mutex mutex;
  std::vector<int> audio;
  std::vector<int> video;
};

struct Recording {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<FrameStreamsReady> streams;
  std::vector<TimelineReset> resets;
  std::vector<StreamEnded> ends;
  std::vector<FrameReady> audio;
  std::vector<FrameReady> video;
  std::vector<std::uint64_t> audio_hashes;
  std::vector<std::uint64_t> video_hashes;
  std::vector<std::string> boundaries;
  std::thread::id audio_thread;
  std::thread::id video_thread;
  int active_audio = 0;
  int active_video = 0;
  bool exclusive_boundaries = true;
  bool ordered_generations = true;
  bool parallel_media = false;
  bool rendezvous = false;
  bool block_video = false;
  bool release_video = false;
  bool video_entered = false;
  bool throw_streams = false;
  bool throw_video = false;
  bool throw_end = false;
  int stop_calls = 0;
  std::uint64_t generation = 0;
  std::shared_ptr<DeliveryOrder> delivery_order;
  int consumer_id = 0;

  bool WaitForEnds(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 10s, [&] { return ends.size() >= count; });
  }
};

class FrameRecorder final : public Sink {
 public:
  explicit FrameRecorder(std::string id, std::shared_ptr<Recording> recording)
      : Sink(std::move(id), SinkMediaType::kFrame),
        recording_(std::move(recording)) {}

  void OnStreamsReady(const FrameStreamsReady& streams) override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    Boundary("streams");
    recording_->streams.push_back(streams);
    recording_->generation = streams.generation;
    if (recording_->throw_streams) {
      throw std::runtime_error("test streams failure");
    }
  }
  void OnAudioFrame(const FrameReady& frame) override { Record(frame, true); }
  void OnVideoFrame(const FrameReady& frame) override { Record(frame, false); }
  void OnTimelineReset(const TimelineReset& reset) override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    Boundary("reset");
    recording_->resets.push_back(reset);
    recording_->generation = reset.generation;
  }
  void OnInputEnded(const StreamEnded& end) override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    Boundary("end");
    recording_->ends.push_back(end);
    recording_->changed.notify_all();
    if (recording_->throw_end) {
      throw std::runtime_error("test end failure");
    }
  }
  void Stop() noexcept override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    if (!stopped_) {
      stopped_ = true;
      Boundary("stop");
      ++recording_->stop_calls;
    }
  }

 private:
  void Boundary(const char* kind) {
    recording_->exclusive_boundaries &=
        recording_->active_audio == 0 && recording_->active_video == 0;
    recording_->boundaries.emplace_back(kind);
  }

  void Record(const FrameReady& frame, bool audio) {
    std::unique_lock<std::mutex> lock(recording_->mutex);
    if (recording_->delivery_order) {
      auto& order = *recording_->delivery_order;
      std::lock_guard<std::mutex> order_lock(order.mutex);
      (audio ? order.audio : order.video).push_back(recording_->consumer_id);
    }
    recording_->ordered_generations &=
        frame.generation == recording_->generation;
    auto& frames = audio ? recording_->audio : recording_->video;
    auto& thread = audio ? recording_->audio_thread : recording_->video_thread;
    auto& active = audio ? recording_->active_audio : recording_->active_video;
    thread = std::this_thread::get_id();
    ++active;
    recording_->parallel_media |=
        recording_->active_audio > 0 && recording_->active_video > 0;
    recording_->changed.notify_all();
    if (recording_->rendezvous && frames.empty()) {
      recording_->changed.wait_for(lock, 2s,
                                   [&] { return recording_->parallel_media; });
    }
    if (!audio && recording_->block_video && !recording_->video_entered) {
      recording_->video_entered = true;
      recording_->changed.notify_all();
      recording_->changed.wait(lock, [&] { return recording_->release_video; });
    }
    --active;
    if (!audio && recording_->throw_video) {
      throw std::runtime_error("test frame failure");
    }
    frames.push_back(frame);
    auto& hashes = audio ? recording_->audio_hashes : recording_->video_hashes;
    hashes.push_back(FrameHash(frame, audio));
    recording_->changed.notify_all();
  }

  std::shared_ptr<Recording> recording_;
  bool stopped_ = false;
};

struct Sample {
  std::vector<StreamInfo> streams;
  std::vector<Packet> packets;
};

Sample ReadSample(AVMediaType media_type = AVMEDIA_TYPE_UNKNOWN) {
  InputFormatContext input(SamplePath());
  input.FindStreamInfo();
  Sample sample;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    const auto* stream = input->streams[i];
    if (media_type == AVMEDIA_TYPE_UNKNOWN ||
        stream->codecpar->codec_type == media_type) {
      sample.streams.push_back({stream->index,
                                CodecParameters(*stream->codecpar),
                                stream->time_base});
    }
  }
  Packet packet;
  while (input.ReadPacket(packet)) {
    for (const auto& stream : sample.streams) {
      if (stream.stream_index == packet->stream_index) {
        sample.packets.push_back(packet);
      }
    }
    packet.Unref();
  }
  return sample;
}

void Feed(DecoderSink& sink, const Sample& sample, std::uint64_t generation) {
  sink.OnStreamsReady({generation, sample.streams});
  for (const auto& packet : sample.packets) {
    sink.OnPacket({generation, packet});
  }
  sink.OnInputEnded({generation, StreamEndReason::kEof});
}

bool WaitForFailure(const DecoderSink& sink) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (sink.state() != PacketSinkState::kFailed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return sink.state() == PacketSinkState::kFailed;
}

void CheckAudio(const Recording& recording) {
  REQUIRE_FALSE(recording.audio.empty());
  const AVChannelLayout* source_layout = nullptr;
  for (const auto& stream : recording.streams.front().source_streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
      source_layout = &stream.codec_parameters.get()->ch_layout;
    }
  }
  REQUIRE(source_layout != nullptr);
  for (const auto& ready : recording.audio) {
    REQUIRE(ready.frame.get());
    CHECK(ready.frame->format == AV_SAMPLE_FMT_FLT);
    CHECK(ready.frame->sample_rate == 48000);
    CHECK(ready.frame->time_base.num == 1);
    CHECK(ready.frame->time_base.den == 48000);
    CHECK(ready.frame->nb_samples > 0);
    CHECK(ready.frame->data[0] != nullptr);
    CHECK(av_channel_layout_compare(&ready.frame->ch_layout, source_layout) ==
          0);
  }
}

}  // namespace

TEST_CASE(
    "DecoderSink decodes real input on parallel workers and retains frames") {
  auto recording = std::make_shared<Recording>();
  recording->rendezvous = true;
  auto config = SoftwareConfig();
  SECTION("zero cache") { config.cache_duration = 0ms; }
  SECTION("one second cache") { config.cache_duration = 1s; }
  auto decoder = std::make_unique<DecoderSink>("decoder", config);
  decoder->AddSink(std::make_unique<FrameRecorder>("recording", recording));
  auto* consumer = decoder.get();
  ZlmInputConfig input_config;
  input_config.url = SamplePath();
  input_config.reconnect_policy.max_retries = 0;
  auto pipeline =
      std::make_unique<Pipeline>(std::make_unique<ZlmInput>(input_config));
  pipeline->AddSink(std::move(decoder));
  pipeline->Start();
  const bool ended = recording->WaitForEnds(1);
  const auto error = consumer->error();
  pipeline->Stop();
  pipeline->Stop();
  pipeline.reset();

  INFO(error);
  REQUIRE(ended);
  REQUIRE(recording->streams.size() == 1);
  CHECK(recording->streams.front().hardware_context == nullptr);
  REQUIRE(recording->ends.size() == 1);
  CHECK(recording->ends.front().reason == StreamEndReason::kEof);
  CHECK(recording->stop_calls == 1);
  CHECK(recording->exclusive_boundaries);
  CHECK(recording->ordered_generations);
  CHECK(recording->parallel_media);
  CHECK(recording->audio_thread != recording->video_thread);
  CHECK(recording->audio_thread != std::this_thread::get_id());
  CHECK(recording->video_thread != std::this_thread::get_id());
  CHECK(recording->boundaries ==
        std::vector<std::string>{"streams", "end", "stop"});
  REQUIRE(recording->video.size() == 20);
  for (const auto& ready : recording->video) {
    REQUIRE(ready.frame.get());
    CHECK(ready.frame->format != AV_PIX_FMT_CUDA);
    CHECK(ready.frame->width == 64);
    CHECK(ready.frame->height == 64);
    CHECK(ready.frame->hw_frames_ctx == nullptr);
    REQUIRE(ready.frame->data[0]);
  }
  CheckAudio(*recording);
  // Re-read shared buffers after Pipeline, decoder and recorder destruction.
  for (std::size_t i = 0; i < recording->video.size(); ++i) {
    CHECK(FrameHash(recording->video[i], false) == recording->video_hashes[i]);
  }
  for (std::size_t i = 0; i < recording->audio.size(); ++i) {
    CHECK(FrameHash(recording->audio[i], true) == recording->audio_hashes[i]);
  }
  std::int64_t samples = 0;
  for (const auto& ready : recording->audio) {
    samples += ready.frame->nb_samples;
  }
  // Cached playback aligns at the first video DTS and removes the earlier
  // AAC preroll packet. Zero-cache delivery retains every input packet.
  const bool cached = config.cache_duration > 0ms;
  CHECK(samples == (cached ? 94 : 95) * 1024);
  CHECK(recording->audio.front().frame->pts == (cached ? 1008 : 0));
  CHECK(av_rescale_q(recording->video.front().frame->pts,
                     recording->video.front().frame->time_base,
                     AVRational{1, 1000}) == 21);
}

TEST_CASE("DecoderSink supports individual tracks and drains decoder output") {
  AVMediaType media_type = AVMEDIA_TYPE_AUDIO;
  SECTION("audio only") { media_type = AVMEDIA_TYPE_AUDIO; }
  SECTION("video only") { media_type = AVMEDIA_TYPE_VIDEO; }
  const auto sample = ReadSample(media_type);
  auto recording = std::make_shared<Recording>();
  DecoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  Feed(sink, sample, 1);
  const bool ended = recording->WaitForEnds(1);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  CHECK(recording->exclusive_boundaries);
  REQUIRE(recording->ends.size() == 1);
  const auto snapshot = sink.GetPerformance();
  CHECK(snapshot.name == "DecoderSink");
  REQUIRE(snapshot.downstream.size() == 1);
  REQUIRE(snapshot.operations.size() == 2);
  const auto& operation =
      snapshot.operations[media_type == AVMEDIA_TYPE_AUDIO ? 0 : 1];
  CHECK(operation.type == (media_type == AVMEDIA_TYPE_AUDIO
                               ? PerformanceType::kAudioDecoder
                               : PerformanceType::kVideoDecoder));
  CHECK(operation.input_unit == PerformanceUnit::kPacket);
  CHECK(operation.input_count == sample.packets.size());
  CHECK(operation.started_calls == sample.packets.size() + 1);
  CHECK(operation.completed_calls == operation.started_calls);
  CHECK(operation.failed_calls == 0);
  CHECK(operation.in_flight == 0);
  if (media_type == AVMEDIA_TYPE_VIDEO) {
    CHECK(operation.output_unit == PerformanceUnit::kFrame);
    CHECK(operation.output_count == recording->video.size());
    CHECK(recording->video.size() == 20);
    CHECK(recording->audio.empty());
  } else {
    CheckAudio(*recording);
    CHECK(recording->video.empty());
  }
}

TEST_CASE("DecoderSink drains delayed 44.1 kHz resampler samples at EOF") {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters.get()->codec_id = AV_CODEC_ID_PCM_F32LE;
  parameters.get()->format = AV_SAMPLE_FMT_FLT;
  parameters.get()->sample_rate = 44100;
  av_channel_layout_default(&parameters.get()->ch_layout, 2);
  Packet packet;
  REQUIRE(av_new_packet(packet.get(), 441 * 2 * sizeof(float)) == 0);
  std::memset(packet->data, 0, packet->size);
  packet->stream_index = 0;
  packet->pts = 0;
  packet->dts = 0;
  packet->duration = 441;
  Sample sample{{{0, std::move(parameters), {1, 44100}}}, {packet}};
  auto recording = std::make_shared<Recording>();
  DecoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  Feed(sink, sample, 1);
  const bool ended = recording->WaitForEnds(1);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(ended);
  CheckAudio(*recording);
  std::int64_t samples = 0;
  for (const auto& ready : recording->audio) {
    samples += ready.frame->nb_samples;
  }
  CHECK(samples == 480);
  const auto snapshot = sink.GetPerformance();
  const auto& audio = snapshot.operations.at(0);
  CHECK(audio.type == PerformanceType::kAudioDecoder);
  CHECK(audio.output_unit == PerformanceUnit::kSample);
  CHECK(audio.input_count == 1);
  CHECK(audio.output_count == 480);
  CHECK(audio.completed_calls == 2);
  CHECK(sink.GetPerformance().operations.at(0).output_count == 480);
  CHECK(recording->audio.size() >= 2);
  CHECK(recording->exclusive_boundaries);
}

TEST_CASE("DecoderSink feeds analysis and passthrough processor branches") {
  struct Calls {
    int audio = 0;
    int video = 0;
    int ends = 0;
    int stops = 0;
  } calls;
  MwStreamerAnalysisProcessorCallbacks callbacks{};
  callbacks.user_context = &calls;
  callbacks.process_audio = [](const MwStreamerAudioFrameView*, void* context) {
    ++static_cast<Calls*>(context)->audio;
  };
  callbacks.process_video = [](const MwStreamerVideoFrameView*, void* context) {
    ++static_cast<Calls*>(context)->video;
  };
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* context) {
    if (reason == kMwStreamerProcessorEndOfInput) {
      ++static_cast<Calls*>(context)->ends;
    }
  };
  callbacks.on_stop = [](void* context) {
    ++static_cast<Calls*>(context)->stops;
  };

  auto original = std::make_shared<Recording>();
  auto forwarded = std::make_shared<Recording>();
  DecoderSink decoder("decoder", SoftwareConfig());
  decoder.AddSink(std::make_unique<AnalysisProcessorSink>(
      "analysis-processor-1", callbacks));
  decoder.AddSink(std::make_unique<FrameRecorder>("original", original));
  auto processor = std::make_unique<TransformProcessorSink>(
      "processor", MwStreamerTransformProcessorCallbacks{});
  processor->AddSink(std::make_unique<FrameRecorder>("forwarded", forwarded));
  decoder.AddSink(std::move(processor));
  Feed(decoder, ReadSample(), 1);
  const bool ended = forwarded->WaitForEnds(1);
  decoder.Stop();

  INFO(decoder.error());
  REQUIRE(ended);
  CHECK(calls.video == 20);
  REQUIRE_FALSE(original->audio.empty());
  CHECK(calls.audio == original->audio.size());
  CHECK(calls.ends == 1);
  CHECK(calls.stops == 1);
  CHECK(forwarded->stop_calls == 1);
  REQUIRE(forwarded->video.size() == original->video.size());
  REQUIRE(forwarded->audio.size() == original->audio.size());
  CheckAudio(*forwarded);
  for (std::size_t i = 0; i < forwarded->video.size(); ++i) {
    CHECK(forwarded->video[i].frame->data[0] ==
          original->video[i].frame->data[0]);
    CHECK(forwarded->video[i].frame->pts == original->video[i].frame->pts);
  }
  for (std::size_t i = 0; i < forwarded->audio.size(); ++i) {
    CHECK(forwarded->audio[i].frame->data[0] ==
          original->audio[i].frame->data[0]);
    CHECK(forwarded->audio[i].frame->pts == original->audio[i].frame->pts);
  }
}

TEST_CASE("DecoderSink reports an in-flight fatal after a local failure") {
  class FailingFrames final : public Sink {
   public:
    FailingFrames(std::shared_future<void> release_audio,
                  std::shared_future<void> release_video)
        : Sink("fatal-recorder", SinkMediaType::kFrame),
          release_audio_(std::move(release_audio)),
          release_video_(std::move(release_video)) {}

    void OnStreamsReady(const FrameStreamsReady&) override {}
    void OnAudioFrame(const FrameReady&) override {
      audio_entered.set_value();
      release_audio_.wait();
      throw std::runtime_error("ordinary audio failure");
    }
    void OnVideoFrame(const FrameReady&) override {
      video_entered.set_value();
      release_video_.wait();
      throw FatalError("late video fatal");
    }
    void OnTimelineReset(const TimelineReset&) override {}
    void OnInputEnded(const StreamEnded&) override {}
    void Stop() noexcept override {}

    std::promise<void> audio_entered;
    std::promise<void> video_entered;

   private:
    std::shared_future<void> release_audio_;
    std::shared_future<void> release_video_;
  };

  std::promise<void> release_audio;
  std::promise<void> release_video;
  auto frames = std::make_unique<FailingFrames>(
      release_audio.get_future().share(), release_video.get_future().share());
  auto audio_entered = frames->audio_entered.get_future();
  auto video_entered = frames->video_entered.get_future();
  std::promise<std::string> fatal;
  auto reported = fatal.get_future();
  DecoderSink sink("sink", SoftwareConfig());
  sink.SetOnFatalError(
      [&](const std::string& error) { fatal.set_value(error); });
  sink.AddSink(std::move(frames));
  Feed(sink, ReadSample(), 1);

  const auto audio_ready = audio_entered.wait_for(5s);
  const auto video_ready = video_entered.wait_for(5s);
  release_audio.set_value();
  const bool local_failed = WaitForFailure(sink);
  release_video.set_value();
  const auto fatal_ready = reported.wait_for(5s);
  sink.Stop();

  REQUIRE(audio_ready == std::future_status::ready);
  REQUIRE(video_ready == std::future_status::ready);
  REQUIRE(local_failed);
  REQUIRE(fatal_ready == std::future_status::ready);
  CHECK(reported.get() == "late video fatal");
  CHECK(sink.error() == "ordinary audio failure");
  CHECK(sink.state() == PacketSinkState::kFailed);
}

TEST_CASE("DecoderSink reconnect resets and serializes generation boundaries") {
  const auto sample = ReadSample();
  auto recording = std::make_shared<Recording>();
  auto config = SoftwareConfig();
  SECTION("zero cache") { config.cache_duration = 0ms; }
  SECTION("one second cache") { config.cache_duration = 1s; }
  DecoderSink sink("sink", config);
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  sink.OnStreamsReady({1, sample.streams});
  for (std::size_t i = 0; i < sample.packets.size() / 2; ++i) {
    sink.OnPacket({1, sample.packets[i]});
  }
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  const bool first_ended = recording->WaitForEnds(1);
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  Feed(sink, sample, 2);
  const bool second_ended = recording->WaitForEnds(2);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(first_ended);
  REQUIRE(second_ended);
  REQUIRE(recording->resets.size() == 1);
  CHECK(recording->resets.front().generation == 2);
  CHECK(recording->resets.front().reason == TimelineResetReason::kReconnect);
  CHECK(recording->exclusive_boundaries);
  CHECK(recording->ordered_generations);
  CHECK(recording->boundaries == std::vector<std::string>{"streams", "end",
                                                          "reset", "streams",
                                                          "end", "stop"});
  std::size_t second_video = 0;
  for (const auto& ready : recording->video) {
    second_video += ready.generation == 2;
  }
  CHECK(second_video == 20);
  REQUIRE(recording->ends.size() == 2);
  CHECK(recording->ends[0].reason == StreamEndReason::kInterrupted);
  CHECK(recording->ends[1].reason == StreamEndReason::kEof);
}

TEST_CASE(
    "DecoderSink preserves queued end boundaries during overlapping "
    "reconnect") {
  const auto sample = ReadSample();
  std::size_t audio_packets = 0;
  for (const auto& packet : sample.packets) {
    for (const auto& stream : sample.streams) {
      audio_packets +=
          packet->stream_index == stream.stream_index &&
          stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO;
    }
  }
  auto recording = std::make_shared<Recording>();
  recording->block_video = true;
  auto config = SoftwareConfig();
  SECTION("zero cache") { config.cache_duration = 0ms; }
  SECTION("one second cache") { config.cache_duration = 1s; }
  DecoderSink sink("sink", config);
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  sink.OnStreamsReady({1, sample.streams});
  for (const auto& packet : sample.packets) {
    sink.OnPacket({1, packet});
  }
  bool video_entered = false;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    video_entered = recording->changed.wait_for(
        lock, 5s, [&] { return recording->video_entered; });
  }
  sink.OnInputEnded({1, StreamEndReason::kInterrupted});
  bool audio_drained = false;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    // FFmpeg removes AAC's priming packet. The audio worker can reach the old
    // End barrier while the video worker is still inside its first callback.
    audio_drained = recording->changed.wait_for(lock, 5s, [&] {
      return audio_packets > 1 && recording->audio.size() >= audio_packets - 1;
    });
  }
  const auto wait_state = [&](PacketSinkState expected) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (sink.state() != expected &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    return sink.state() == expected;
  };
  const bool draining = wait_state(PacketSinkState::kDraining);
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  sink.OnStreamsReady({2, sample.streams});
  // Running proves the replacement configuration (including packet removal)
  // has executed before we release the old video callback. Old End controls
  // must remain queued on both tracks regardless of their barrier progress.
  const bool replacement_configured = wait_state(PacketSinkState::kRunning);
  for (const auto& packet : sample.packets) {
    sink.OnPacket({2, packet});
  }
  sink.OnInputEnded({2, StreamEndReason::kEof});
  bool old_end_waited = false;
  {
    std::lock_guard<std::mutex> lock(recording->mutex);
    old_end_waited = recording->ends.empty();
    recording->release_video = true;
    recording->changed.notify_all();
  }
  const bool ended = recording->WaitForEnds(2);
  sink.Stop();
  INFO(sink.error());
  REQUIRE(video_entered);
  REQUIRE(audio_drained);
  REQUIRE(draining);
  REQUIRE(replacement_configured);
  CHECK(old_end_waited);
  REQUIRE(ended);
  CHECK(recording->exclusive_boundaries);
  CHECK(recording->ordered_generations);
  CHECK(recording->boundaries == std::vector<std::string>{"streams", "end",
                                                          "reset", "streams",
                                                          "end", "stop"});
  REQUIRE(recording->ends.size() == 2);
  CHECK(recording->ends[0].generation == 1);
  CHECK(recording->ends[0].reason == StreamEndReason::kInterrupted);
  CHECK(recording->ends[1].generation == 2);
  CHECK(recording->ends[1].reason == StreamEndReason::kEof);
  REQUIRE(recording->streams.size() == 2);
  CHECK(recording->streams[1].generation == 2);
  REQUIRE(recording->resets.size() == 1);
  CHECK(recording->resets.front().generation == 2);
  std::size_t second_video = 0;
  for (const auto& frame : recording->video) {
    second_video += frame.generation == 2;
  }
  CHECK(second_video == 20);
  CHECK(recording->stop_calls == 1);
}

TEST_CASE("Pipeline stop waits for decoder media work and stops output once") {
  auto recording = std::make_shared<Recording>();
  recording->block_video = true;
  ZlmInputConfig input_config;
  input_config.url = SamplePath();
  input_config.reconnect_policy.max_retries = 0;
  Pipeline pipeline(std::make_unique<ZlmInput>(input_config));
  auto decoder = std::make_unique<DecoderSink>("decoder", SoftwareConfig());
  decoder->AddSink(std::make_unique<FrameRecorder>("recording", recording));
  pipeline.AddSink(std::move(decoder));
  pipeline.Start();
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    entered = recording->changed.wait_for(
        lock, 5s, [&] { return recording->video_entered; });
  }
  auto stopped = std::async(std::launch::async, [&] { pipeline.Stop(); });
  const auto blocked = stopped.wait_for(100ms);
  {
    std::lock_guard<std::mutex> lock(recording->mutex);
    recording->release_video = true;
    recording->changed.notify_all();
  }
  const auto complete = stopped.wait_for(5s);
  REQUIRE(entered);
  CHECK(blocked == std::future_status::timeout);
  REQUIRE(complete == std::future_status::ready);
  stopped.get();
  const auto video_count = recording->video.size();
  pipeline.Stop();
  CHECK(recording->video.size() == video_count);
  CHECK(recording->stop_calls == 1);
  CHECK(recording->exclusive_boundaries);
}

TEST_CASE(
    "DecoderSink isolates downstream exceptions and stops without deadlock") {
  auto recording = std::make_shared<Recording>();
  auto first = std::make_shared<Recording>();
  auto last = std::make_shared<Recording>();
  auto config = SoftwareConfig();
  SECTION("initial streams callback throws") {
    recording->throw_streams = true;
  }
  SECTION("decoder initialization fails") {
    config.video_decoder.decoder_name = "mw_missing_decoder";
  }
  SECTION("media callback throws") { recording->throw_video = true; }
  SECTION("end callback throws") { recording->throw_end = true; }
  DecoderSink sink("sink", config);
  sink.AddSink(std::make_unique<FrameRecorder>("first", first));
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  sink.AddSink(std::make_unique<FrameRecorder>("last", last));
  Feed(sink, ReadSample(), 1);
  const bool failed = WaitForFailure(sink);
  sink.Stop();
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording->stop_calls == 1);
  CHECK(first->stop_calls == 1);
  CHECK(last->stop_calls == 1);
  CHECK(recording->exclusive_boundaries);
  CHECK(first->exclusive_boundaries);
  CHECK(last->exclusive_boundaries);
}

TEST_CASE(
    "DecoderSink fans out shared frames and boundaries in registration order") {
  const auto sample = ReadSample();
  auto first = std::make_shared<Recording>();
  auto second = std::make_shared<Recording>();
  auto order = std::make_shared<DeliveryOrder>();
  first->delivery_order = order;
  second->delivery_order = order;
  first->consumer_id = 1;
  second->consumer_id = 2;
  auto sink = std::make_unique<DecoderSink>("sink", SoftwareConfig());
  Sink& fanout = *sink;
  fanout.AddSink(std::make_unique<FrameRecorder>("first", first));
  fanout.AddSink(std::make_unique<FrameRecorder>("second", second));
  sink->OnStreamsReady({1, sample.streams});
  for (const auto& packet : sample.packets) {
    sink->OnPacket({1, packet});
  }
  sink->OnInputEnded({1, StreamEndReason::kInterrupted});
  const bool interrupted = second->WaitForEnds(1);
  sink->OnTimelineReset({2, TimelineResetReason::kReconnect, std::nullopt});
  Feed(*sink, sample, 2);
  const bool ended = second->WaitForEnds(2);
  const auto error = sink->error();
  sink->Stop();
  sink.reset();

  INFO(error);
  REQUIRE(interrupted);
  REQUIRE(ended);
  for (const auto& recording : {first, second}) {
    CHECK(recording->exclusive_boundaries);
    CHECK(recording->ordered_generations);
    REQUIRE(recording->streams.size() == 2);
    CHECK(recording->streams[0].generation == 1);
    CHECK(recording->streams[1].generation == 2);
    REQUIRE(recording->resets.size() == 1);
    CHECK(recording->resets[0].generation == 2);
    REQUIRE(recording->ends.size() == 2);
    CHECK(recording->ends[0].reason == StreamEndReason::kInterrupted);
    CHECK(recording->ends[1].reason == StreamEndReason::kEof);
    CHECK(recording->stop_calls == 1);
    CHECK(recording->boundaries == std::vector<std::string>{"streams", "end",
                                                            "reset", "streams",
                                                            "end", "stop"});
  }
  const auto check_frames = [](const auto& left, const auto& right,
                               const auto& deliveries) {
    REQUIRE_FALSE(left.empty());
    REQUIRE(left.size() == right.size());
    REQUIRE(deliveries.size() == left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
      CHECK(deliveries[i * 2] == 1);
      CHECK(deliveries[i * 2 + 1] == 2);
      CHECK(left[i].generation == right[i].generation);
      CHECK(left[i].frame->pts == right[i].frame->pts);
      CHECK(left[i].frame->data[0] == right[i].frame->data[0]);
      REQUIRE(left[i].frame->buf[0]);
      REQUIRE(right[i].frame->buf[0]);
      CHECK(left[i].frame->buf[0]->buffer == right[i].frame->buf[0]->buffer);
    }
  };
  CHECK(first->video.size() == 40);
  check_frames(first->audio, second->audio, order->audio);
  check_frames(first->video, second->video, order->video);
  CHECK(first->audio_hashes == second->audio_hashes);
  CHECK(first->video_hashes == second->video_hashes);
}

TEST_CASE("DecoderSink only accepts frame consumers before input or Stop") {
  auto recording = std::make_shared<Recording>();
  DecoderSink sink("sink", SoftwareConfig());
  CHECK_THROWS_AS(sink.AddSink(nullptr), std::invalid_argument);
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  SECTION("first source submission locks registration synchronously") {
    sink.OnStreamsReady({1, ReadSample().streams});
  }
  SECTION("Stop locks registration before input starts") { sink.Stop(); }
  CHECK_THROWS_AS(
      sink.AddSink(std::make_unique<FrameRecorder>("recording", recording)),
      std::logic_error);
  sink.Stop();
  CHECK(recording->stop_calls == 1);
}

TEST_CASE("DecoderSink fails source initialization without frame consumers") {
  DecoderSink sink("sink", SoftwareConfig());
  sink.OnStreamsReady({1, ReadSample().streams});
  const bool failed = WaitForFailure(sink);
  sink.Stop();
  REQUIRE(failed);
  CHECK_FALSE(sink.error().empty());
}

TEST_CASE("DecoderSink retains PacketQueue failures across Stop") {
  auto recording = std::make_shared<Recording>();
  DecoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  std::size_t ready_count = 0;
  SECTION("queue fails before a consumer receives streams") {
    sink.OnStreamsReady({1, {}});
  }
  SECTION("queue fails after a consumer receives streams") {
    const auto streams = ReadSample().streams;
    sink.OnStreamsReady({1, streams});
    sink.OnStreamsReady({2, streams});
    ready_count = 1;
  }
  const bool failed = WaitForFailure(sink);
  sink.Stop();
  REQUIRE(failed);
  CHECK(sink.state() == PacketSinkState::kFailed);
  CHECK_FALSE(sink.error().empty());
  CHECK(recording->streams.size() == ready_count);
  CHECK(recording->stop_calls == 1);
  CHECK(recording->exclusive_boundaries);
}

TEST_CASE(
    "DecoderSink cancels pending decode work when its queue fails after EOF") {
  const auto sample = ReadSample();
  std::size_t audio_packets = 0;
  for (const auto& packet : sample.packets) {
    for (const auto& stream : sample.streams) {
      audio_packets +=
          packet->stream_index == stream.stream_index &&
          stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO;
    }
  }
  auto recording = std::make_shared<Recording>();
  recording->block_video = true;
  DecoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  Feed(sink, sample, 1);
  bool video_entered = false;
  bool audio_drained = false;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    video_entered = recording->changed.wait_for(
        lock, 5s, [&] { return recording->video_entered; });
    // AAC's priming packet produces no output. The remaining audio reaches
    // the EOF barrier while the first video callback is still blocked.
    audio_drained = recording->changed.wait_for(lock, 5s, [&] {
      return audio_packets > 1 && recording->audio.size() >= audio_packets - 1;
    });
  }
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (sink.state() != PacketSinkState::kDraining &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const bool draining = sink.state() == PacketSinkState::kDraining;
  // The queue already delivered generation 1's EOF. It cannot report this
  // configuration failure by sending another End for the same generation.
  sink.OnStreamsReady({2, sample.streams});
  const bool failed = WaitForFailure(sink);
  bool stale_end_delivered = false;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    recording->release_video = true;
    recording->changed.notify_all();
    // Let the workers react before Stop can discard their pending work and
    // conceal failure propagation that only updates the public snapshot.
    stale_end_delivered = recording->changed.wait_for(
        lock, 300ms, [&] { return !recording->ends.empty(); });
  }
  const auto stop_time = std::chrono::steady_clock::now();
  sink.Stop();
  const auto stop_duration = std::chrono::steady_clock::now() - stop_time;
  INFO(sink.error());
  REQUIRE(video_entered);
  REQUIRE(audio_drained);
  REQUIRE(draining);
  REQUIRE(failed);
  CHECK_FALSE(stale_end_delivered);
  CHECK(recording->video.size() == 1);
  CHECK(recording->ends.empty());
  CHECK(sink.state() == PacketSinkState::kFailed);
  CHECK_FALSE(sink.error().empty());
  CHECK(stop_duration < 2s);
  CHECK(recording->stop_calls == 1);
  CHECK(recording->exclusive_boundaries);
}

TEST_CASE("DecoderSink statistics exclude blocked downstream processing") {
  const auto sample = ReadSample(AVMEDIA_TYPE_VIDEO);
  auto recording = std::make_shared<Recording>();
  recording->block_video = true;
  DecoderSink sink("sink", SoftwareConfig());
  sink.AddSink(std::make_unique<FrameRecorder>("recording", recording));
  Feed(sink, sample, 1);
  bool entered;
  {
    std::unique_lock<std::mutex> lock(recording->mutex);
    entered = recording->changed.wait_for(
        lock, 5s, [&] { return recording->video_entered; });
  }
  const auto during = sink.GetPerformance();
  const auto blocked_at = std::chrono::steady_clock::now();
  if (entered) {
    std::this_thread::sleep_for(300ms);
  }
  const auto blocked_time = std::chrono::steady_clock::now() - blocked_at;
  {
    std::lock_guard<std::mutex> lock(recording->mutex);
    recording->release_video = true;
    recording->changed.notify_all();
  }
  const bool ended = recording->WaitForEnds(1);
  sink.Stop();
  REQUIRE(entered);
  REQUIRE(ended);
  const auto& active = during.operations.at(1);
  CHECK(active.in_flight == 1);
  CHECK(active.output_count == 1);
  CHECK(active.input_count < sample.packets.size());
  const auto after = sink.GetPerformance();
  const auto& video = after.operations.at(1);
  CHECK(video.input_count == sample.packets.size());
  CHECK(video.output_count == recording->video.size());
  CHECK(video.total_time < blocked_time);
  CHECK(video.in_flight == 0);
}

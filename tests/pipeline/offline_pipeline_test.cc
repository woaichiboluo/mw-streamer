#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mw/streamer/decoder/decoder_sink.h"
#include "mw/streamer/input/file_input.h"
#include "mw/streamer/pipeline/pipeline.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::DecoderSink;
using mw::streamer::DecoderSinkConfig;
using mw::streamer::FileInput;
using mw::streamer::FileInputConfig;
using mw::streamer::InputState;
using mw::streamer::FrameReady;
using mw::streamer::FrameStreamsReady;
using mw::streamer::StreamEnded;
using mw::streamer::StreamEndReason;
using mw::streamer::FatalError;
using mw::streamer::Sink;
using mw::streamer::SinkMediaType;
using namespace mw::streamer;

struct FileFrames {
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<int> video{0};
  std::atomic<int> audio_samples{0};
  bool block = false;
  bool entered = false;
  bool released = false;
  bool ended = false;
  bool fatal = false;
  int video_frame_rate_num = 0;
  int video_frame_rate_den = 1;
  StreamEndReason reason = StreamEndReason::kStopped;
};

class FileCounter final : public Sink {
 public:
  explicit FileCounter(FileFrames& frames)
      : Sink("counter", SinkMediaType::kFrame), frames_(frames) {}
  void OnStreamsReady(const FrameStreamsReady& streams) override {
    for (const auto& stream : streams.source_streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
        continue;
      }
      std::lock_guard<std::mutex> lock(frames_.mutex);
      frames_.video_frame_rate_num = parameters->framerate.num;
      frames_.video_frame_rate_den = parameters->framerate.den;
      return;
    }
  }
  void OnAudioFrame(const FrameReady& frame) override {
    Gate();
    frames_.audio_samples += frame.frame->nb_samples;
  }
  void OnVideoFrame(const FrameReady&) override {
    Gate();
    ++frames_.video;
  }
  void OnInputEnded(const StreamEnded& end) override {
    std::lock_guard<std::mutex> lock(frames_.mutex);
    frames_.ended = true;
    frames_.reason = end.reason;
    frames_.changed.notify_all();
  }

 private:
  void Gate() {
    if (frames_.block) {
      std::unique_lock<std::mutex> lock(frames_.mutex);
      frames_.entered = true;
      frames_.changed.notify_all();
      frames_.changed.wait(lock, [&] { return frames_.released; });
    }
    if (frames_.fatal) {
      throw FatalError("offline processor failed");
    }
    // With one queued packet per track, reading cannot outrun this consumer.
    std::this_thread::sleep_for(2ms);
  }
  FileFrames& frames_;
};

std::string FilePath(const char* name) {
  return std::string(MW_DECODER_SINK_TEST_DATA_DIR) + "/" + name;
}

std::unique_ptr<DecoderSink> MakeDecoder(FileFrames& frames) {
  DecoderSinkConfig config;
  config.audio_decode_queue_capacity = 1;
  config.video_decode_queue_capacity = 1;
  config.video_decoder.backend =
      mw::streamer::VideoDecoderBackend::kSoftware;
  auto decoder = std::make_unique<DecoderSink>("decoder", config);
  decoder->AddSink(std::make_unique<FileCounter>(frames));
  return decoder;
}

void Release(FileFrames& frames) {
  std::lock_guard<std::mutex> lock(frames.mutex);
  frames.released = true;
  frames.changed.notify_all();
}

}  // namespace

TEST_CASE(
    "Offline input preserves all decoded frames under bounded backpressure") {
  const char* name = "h264_aac.mp4";
  SECTION("H264") {}
  SECTION("HEVC with B frames") { name = "h265_aac.mp4"; }
  FileFrames frames;
  Pipeline pipeline(
      std::make_unique<FileInput>(FileInputConfig{FilePath(name)}));
  auto decoder = MakeDecoder(frames);
  auto* decoding = decoder.get();
  pipeline.AddSink(std::move(decoder));
  pipeline.Start();
  bool ended;
  {
    std::unique_lock<std::mutex> lock(frames.mutex);
    ended = frames.changed.wait_for(lock, 10s, [&] { return frames.ended; });
  }
  const auto error = decoding->error();
  pipeline.Stop();
  INFO(error);
  REQUIRE(ended);
  CHECK(error.empty());
  CHECK(frames.reason == StreamEndReason::kEof);
  CHECK(frames.video == 20);
  CHECK(frames.video_frame_rate_num == 10);
  CHECK(frames.video_frame_rate_den == 1);
  // FFmpeg packet side data removes AAC encoder priming before callbacks.
  CHECK(frames.audio_samples == 94 * 1024);
}

TEST_CASE("Offline Stop releases a producer blocked by full decode queues") {
  FileFrames frames;
  frames.block = true;
  auto input =
      std::make_unique<FileInput>(FileInputConfig{FilePath("h264_aac.mp4")});
  auto* source = input.get();
  Pipeline pipeline(std::move(input));
  pipeline.AddSink(MakeDecoder(frames));
  pipeline.Start();
  bool entered;
  {
    std::unique_lock<std::mutex> lock(frames.mutex);
    entered = frames.changed.wait_for(lock, 5s, [&] { return frames.entered; });
  }
  if (!entered) {
    Release(frames);
    pipeline.Stop();
  }
  REQUIRE(entered);
  std::this_thread::sleep_for(50ms);
  const auto before = source->GetPerformance().operations.front().output_count;
  std::this_thread::sleep_for(50ms);
  CHECK(source->GetPerformance().operations.front().output_count == before);
  CHECK(before < 115);
  CHECK(source->state() == InputState::kReady);

  auto stopped = std::async(std::launch::async, [&] { pipeline.Stop(); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (source->state() != InputState::kStopped &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  // Input can join before the blocked business callback returns. Stop still
  // waits for that callback and releases its borrowed state only afterwards.
  CHECK(source->state() == InputState::kStopped);
  CHECK(stopped.wait_for(20ms) == std::future_status::timeout);
  Release(frames);
  REQUIRE(stopped.wait_for(5s) == std::future_status::ready);
  stopped.get();
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK_FALSE(frames.ended);
}

TEST_CASE("Offline downstream Fatal stops the complete Pipeline") {
  FileFrames frames;
  frames.fatal = true;
  Pipeline pipeline(
      std::make_unique<FileInput>(FileInputConfig{FilePath("h264_aac.mp4")}));
  pipeline.AddSink(MakeDecoder(frames));
  pipeline.Start();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (pipeline.state() != PipelineState::kFailed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.error() == "offline processor failed");
}

#include "mw/streamer/pipeline/offline_pipeline.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvMediaSourceState;
using mw::streamer::OfflinePipeline;
using mw::streamer::OfflinePipelineConfig;
using mw::streamer::OfflinePipelineState;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using Runtime = std::unique_ptr<mw::streamer::MwStreamerContext,
                                decltype(&mw::streamer::Shutdown)>;

Runtime InitRuntime() {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 1;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console.enabled = false;
  return Runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
}

std::string SamplePath(const std::string& filename = "h264_aac.mp4") {
  return (fs::u8path(MW_STREAMER_TEST_DATA_DIR) / filename).generic_u8string();
}

OfflinePipelineConfig Config() {
  OfflinePipelineConfig config;
  config.input.packet_capacity = 1;
  config.video_decoder.packet_capacity = 1;
  config.video_decoder.frame_capacity = 1;
  config.video_decoder.thread_count = 1;
  config.audio_decoder = config.video_decoder;
  return config;
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

bool WaitEnded(OfflinePipeline& pipeline) {
  return WaitFor([&] {
    const auto state = pipeline.state();
    return state == OfflinePipelineState::kEnded ||
           state == OfflinePipelineState::kFailed;
  });
}

ffmpeg::Frame InvalidFrame() {
  ffmpeg::Frame frame;
  const auto owned = std::move(frame);
  return frame;
}

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

using FrameTimes = std::array<std::vector<std::int64_t>, 2>;

// Decode independently, including decoder drain, to catch lost delayed frames.
FrameTimes NativeFrameTimes(const std::string& path) {
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  std::array<std::unique_ptr<AVCodecContext, CodecCloser>, 2> decoders;
  std::array<int, 2> indices{-1, -1};
  for (std::size_t index = 0; index < decoders.size(); ++index) {
    const auto type = index == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    indices[index] = av_find_best_stream(input.get(), type, -1, -1, nullptr, 0);
    if (indices[index] < 0) continue;
    const auto* stream = input->streams[indices[index]];
    const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    REQUIRE(codec != nullptr);
    decoders[index].reset(avcodec_alloc_context3(codec));
    REQUIRE(decoders[index] != nullptr);
    REQUIRE(avcodec_parameters_to_context(decoders[index].get(),
                                          stream->codecpar) >= 0);
    decoders[index]->pkt_timebase = stream->time_base;
    decoders[index]->thread_count = 1;
    REQUIRE(avcodec_open2(decoders[index].get(), codec, nullptr) >= 0);
  }
  FrameTimes times;
  const auto receive = [&](std::size_t index) {
    for (;;) {
      ffmpeg::Frame frame;
      const int result =
          avcodec_receive_frame(decoders[index].get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      times[index].push_back(frame->best_effort_timestamp);
    }
  };
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(input.get(), packet.get())) >= 0) {
    for (std::size_t index = 0; index < decoders.size(); ++index) {
      if (packet->stream_index != indices[index]) continue;
      REQUIRE(avcodec_send_packet(decoders[index].get(), packet.get()) >= 0);
      receive(index);
    }
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  for (std::size_t index = 0; index < decoders.size(); ++index) {
    if (!decoders[index]) continue;
    REQUIRE(avcodec_send_packet(decoders[index].get(), nullptr) >= 0);
    receive(index);
  }
  return times;
}

class TemporaryAudio final {
 public:
  TemporaryAudio()
      : path_(
            fs::temp_directory_path() /
            ("mw-offline-audio-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".wav")) {
    AVFormatContext* output = nullptr;
    REQUIRE(avformat_alloc_output_context2(&output, nullptr, "wav",
                                           path().c_str()) >= 0);
    REQUIRE(output != nullptr);
    struct OutputCloser {
      void operator()(AVFormatContext* context) const {
        if (context->pb) avio_closep(&context->pb);
        avformat_free_context(context);
      }
    };
    std::unique_ptr<AVFormatContext, OutputCloser> owner(output);
    auto* stream = avformat_new_stream(output, nullptr);
    REQUIRE(stream != nullptr);
    stream->time_base = {1, 48000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->sample_rate = 48000;
    stream->codecpar->format = AV_SAMPLE_FMT_S16;
    stream->codecpar->bits_per_coded_sample = 16;
    stream->codecpar->block_align = 2;
    av_channel_layout_default(&stream->codecpar->ch_layout, 1);
    REQUIRE(avio_open(&output->pb, path().c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output, nullptr) >= 0);
    ffmpeg::Packet packet;
    REQUIRE(av_new_packet(packet.get(), 48000 * 2) >= 0);
    packet->stream_index = stream->index;
    packet->pts = packet->dts = 0;
    packet->duration = 48000;
    for (int index = 0; index < packet->size; ++index) {
      packet->data[index] = static_cast<std::uint8_t>(index % 127);
    }
    REQUIRE(av_interleaved_write_frame(output, packet.get()) >= 0);
    REQUIRE(av_write_trailer(output) >= 0);
  }
  ~TemporaryAudio() {
    std::error_code error;
    fs::remove(path_, error);
  }
  std::string path() const { return path_.generic_u8string(); }

 private:
  fs::path path_;
};

TEST_CASE("OfflinePipeline consumes all tracks before resetting once",
          "[offline_pipeline][integration]") {
  const auto runtime = InitRuntime();
  TemporaryAudio audio;
  std::string path;
  SECTION("audio and video") { path = SamplePath(); }
  SECTION("video only") { path = SamplePath("h264_video.mp4"); }
  SECTION("audio only") { path = audio.path(); }
  const auto expected = NativeFrameTimes(path);
  FrameTimes actual;
  FrameTimes at_reset;
  std::vector<std::string> events;
  std::atomic<int> active{0};
  std::atomic<bool> overlap{false};
  OfflinePipeline pipeline(Config());
  pipeline.media_filter().SetOnStart([&] { events.emplace_back("start"); });
  const auto consume = [&](const ffmpeg::Frame& frame, std::size_t track) {
    if (active.fetch_add(1) != 0) overlap.store(true);
    actual[track].push_back(frame->pts);
    std::this_thread::yield();
    active.fetch_sub(1);
    return InvalidFrame();
  };
  pipeline.media_filter().SetOnVideo(
      [&](const ffmpeg::Frame& frame) { return consume(frame, 0); });
  pipeline.media_filter().SetOnAudio(
      [&](const ffmpeg::Frame& frame) { return consume(frame, 1); });
  pipeline.media_filter().SetOnReset([&] {
    at_reset = actual;
    events.emplace_back("reset");
  });
  pipeline.media_filter().SetOnStop([&] { events.emplace_back("stop"); });
  REQUIRE(pipeline.Start(path));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  REQUIRE(pipeline.state() == OfflinePipelineState::kEnded);
  CHECK(pipeline.input_status().state == AvMediaSourceState::kEnded);
  CHECK_FALSE(pipeline.error());
  CHECK(actual == expected);
  CHECK(at_reset == expected);
  CHECK_FALSE(overlap.load());
  CHECK(events == std::vector<std::string>{"start", "reset", "stop"});
}

TEST_CASE("OfflinePipeline runs faster than the file media clock",
          "[offline_pipeline][pacing]") {
  const auto runtime = InitRuntime();
  OfflinePipeline pipeline(Config());
  std::atomic<std::size_t> frames{0};
  pipeline.media_filter().SetOnVideo([&](const ffmpeg::Frame&) {
    frames.fetch_add(1);
    return ffmpeg::Frame{};
  });
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(pipeline.Start(SamplePath("h264_video.mp4")));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  CHECK(pipeline.state() == OfflinePipelineState::kEnded);
  CHECK(frames.load() > 0);
  CHECK(std::chrono::steady_clock::now() - started < 1500ms);
}

TEST_CASE("OfflinePipeline cancellation waits for the active business callback",
          "[offline_pipeline][backpressure]") {
  const auto runtime = InitRuntime();
  // Declare the gate before the pipeline so captured state outlives Stop.
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool released = false;
  std::atomic<std::size_t> frames{0};
  std::atomic<std::size_t> packets{0};
  std::atomic<int> resets{0};
  std::atomic<int> stops{0};
  OfflinePipeline pipeline(Config());
  pipeline.SetOnInputPacket(
      [&](const ffmpeg::Packet&) { packets.fetch_add(1); });
  const auto consume = [&](const ffmpeg::Frame&) {
    frames.fetch_add(1);
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
    return ffmpeg::Frame{};
  };
  pipeline.media_filter().SetOnVideo(consume);
  pipeline.media_filter().SetOnAudio(consume);
  pipeline.media_filter().SetOnReset([&] { resets.fetch_add(1); });
  pipeline.media_filter().SetOnStop([&] { stops.fetch_add(1); });
  REQUIRE(pipeline.Start(SamplePath()));
  bool ready = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready = changed.wait_for(lock, 5s, [&] { return entered; });
  }
  // Always release before asserting so an unexpected failure cannot hang Stop.
  std::this_thread::sleep_for(100ms);
  const auto blocked_frames = frames.load();
  const auto blocked_packets = packets.load();
  pipeline.RequestStop();
  auto stopped = std::async(std::launch::async, [&] { pipeline.Stop(); });
  const bool waited = stopped.wait_for(50ms) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  changed.notify_all();
  stopped.get();
  REQUIRE(ready);
  CHECK(waited);
  CHECK(blocked_frames == 1);
  CHECK(blocked_packets > 0);
  CHECK(blocked_packets < 32);
  CHECK(frames.load() == 1);
  CHECK(resets.load() == 0);
  CHECK(stops.load() == 1);
  CHECK(pipeline.state() == OfflinePipelineState::kStopped);
}

TEST_CASE("OfflinePipeline callbacks may query and request cancellation",
          "[offline_pipeline][lifecycle]") {
  const auto runtime = InitRuntime();
  std::atomic<int> frames{0};
  OfflinePipeline pipeline(Config());
  pipeline.media_filter().SetOnVideo([&](const ffmpeg::Frame&) {
    static_cast<void>(pipeline.status());
    static_cast<void>(pipeline.input_status());
    static_cast<void>(pipeline.error());
    frames.fetch_add(1);
    pipeline.RequestStop();
    return ffmpeg::Frame{};
  });
  REQUIRE(pipeline.Start(SamplePath("h264_video.mp4")));
  REQUIRE(WaitFor([&] { return frames.load() != 0; }));
  pipeline.Stop();
  CHECK(frames.load() == 1);
  CHECK(pipeline.state() == OfflinePipelineState::kStopped);
}

TEST_CASE("OfflinePipeline reports an input failure once and can restart",
          "[offline_pipeline][error]") {
  const auto runtime = InitRuntime();
  OfflinePipeline pipeline(Config());
  std::atomic<int> errors{0};
  pipeline.SetOnError([&](std::exception_ptr error) {
    if (error) errors.fetch_add(1);
    static_cast<void>(pipeline.status());
  });
  static_cast<void>(pipeline.Start(SamplePath("missing_offline_input.mp4")));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  CHECK(pipeline.state() == OfflinePipelineState::kFailed);
  CHECK(pipeline.error() != nullptr);
  CHECK(errors.load() == 1);
  REQUIRE(pipeline.Start(SamplePath("h264_video.mp4")));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  CHECK(pipeline.state() == OfflinePipelineState::kEnded);
  CHECK_FALSE(pipeline.error());
  CHECK(errors.load() == 1);
}

TEST_CASE("OfflinePipeline reports corrupt local files asynchronously",
          "[offline_pipeline][error]") {
  const auto runtime = InitRuntime();
  TemporaryAudio corrupt;
  fs::resize_file(corrupt.path(), 0);
  std::atomic<int> errors{0};
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  std::atomic<int> resets{0};
  std::atomic<int> packets{0};
  OfflinePipeline pipeline(Config());
  pipeline.SetOnError([&](std::exception_ptr) {
    static_cast<void>(pipeline.status());
    static_cast<void>(pipeline.input_status());
    static_cast<void>(pipeline.error());
    pipeline.RequestStop();
    errors.fetch_add(1);
  });
  pipeline.SetOnInputPacket(
      [&](const ffmpeg::Packet&) { packets.fetch_add(1); });
  pipeline.media_filter().SetOnStart([&] { starts.fetch_add(1); });
  pipeline.media_filter().SetOnStop([&] { stops.fetch_add(1); });
  pipeline.media_filter().SetOnReset([&] { resets.fetch_add(1); });
  REQUIRE(pipeline.Start(corrupt.path()));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  CHECK(pipeline.state() == OfflinePipelineState::kFailed);
  CHECK(pipeline.error() != nullptr);
  CHECK(errors.load() == 1);
  CHECK(starts.load() == 1);
  CHECK(stops.load() == 1);
  CHECK(resets.load() == 0);
  CHECK(packets.load() == 0);
}

TEST_CASE("OfflinePipeline refuses network inputs without reading packets",
          "[offline_pipeline][configuration]") {
  const auto runtime = InitRuntime();
  for (const auto* url :
       {"https://127.0.0.1/file.mp4", "rtsp://127.0.0.1/live/test", ""}) {
    CAPTURE(url);
    OfflinePipeline pipeline(Config());
    std::atomic<int> errors{0};
    std::atomic<int> packets{0};
    pipeline.SetOnError([&](std::exception_ptr) { errors.fetch_add(1); });
    pipeline.SetOnInputPacket(
        [&](const ffmpeg::Packet&) { packets.fetch_add(1); });
    CHECK_FALSE(pipeline.Start(url));
    pipeline.Stop();
    CHECK(pipeline.state() == OfflinePipelineState::kFailed);
    CHECK(errors.load() == 1);
    CHECK(packets.load() == 0);
  }
}

TEST_CASE("OfflinePipeline resets only after success and always stops filters",
          "[offline_pipeline][error]") {
  const auto runtime = InitRuntime();
  bool reset_failure = false;
  SECTION("frame callback fails") {}
  SECTION("reset callback fails") { reset_failure = true; }
  std::atomic<int> errors{0};
  std::atomic<int> resets{0};
  std::atomic<int> stops{0};
  OfflinePipeline pipeline(Config());
  pipeline.SetOnError([&](std::exception_ptr) { errors.fetch_add(1); });
  pipeline.media_filter().SetOnVideo([&](const ffmpeg::Frame&) {
    if (!reset_failure) throw std::runtime_error("offline business failed");
    return ffmpeg::Frame{};
  });
  pipeline.media_filter().SetOnReset([&] {
    resets.fetch_add(1);
    if (reset_failure) throw std::runtime_error("offline reset failed");
  });
  pipeline.media_filter().SetOnStop([&] { stops.fetch_add(1); });
  REQUIRE(pipeline.Start(SamplePath("h264_video.mp4")));
  REQUIRE(WaitEnded(pipeline));
  pipeline.Stop();
  CHECK(pipeline.state() == OfflinePipelineState::kFailed);
  CHECK(errors.load() == 1);
  CHECK(resets.load() == (reset_failure ? 1 : 0));
  CHECK(stops.load() == 1);
  REQUIRE(pipeline.error() != nullptr);
  CHECK_THROWS_AS(std::rethrow_exception(pipeline.error()), std::runtime_error);
}

}  // namespace

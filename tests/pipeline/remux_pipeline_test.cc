#include "mw/streamer/pipeline/remux_pipeline.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
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
#include <libavutil/mem.h>
}

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/recorder/recorder.h"
#include "streamer/pipeline/audio_probe_timeout.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvInputProxyState;
using mw::streamer::RecorderState;
using mw::streamer::RemuxPipeline;
using mw::streamer::RemuxPipelineConfig;
using mw::streamer::RemuxPipelineState;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using Runtime = std::unique_ptr<mw::streamer::MwStreamerContext,
                                decltype(&mw::streamer::Shutdown)>;

Runtime InitRuntime() {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console.enabled = false;
  return Runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
}

std::string SamplePath(const std::string& filename = "h264_aac.mp4") {
  return (fs::u8path(MW_STREAMER_TEST_DATA_DIR) / filename).generic_u8string();
}

RemuxPipelineConfig Config(double speed = 1.0) {
  RemuxPipelineConfig config;
  config.input.max_retries = 0;
  config.input.input.packet_capacity = 2;
  config.speed = speed;
  return config;
}

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(fs::temp_directory_path() /
              ("mw-remux-pipeline-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    fs::create_directories(path_);
  }
  ~OutputDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  std::string File(const std::string& name) const {
    return (path_ / name).generic_u8string();
  }

 private:
  fs::path path_;
};

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatCloser> OpenMedia(
    const std::string& path) {
  AVFormatContext* raw = nullptr;
  REQUIRE(avformat_open_input(&raw, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, FormatCloser> input(raw);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  return input;
}

std::vector<ffmpeg::Packet> ReadPackets(AVFormatContext& input) {
  std::vector<ffmpeg::Packet> packets;
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(&input, packet.get())) >= 0) {
    packet->time_base = input.streams[packet->stream_index]->time_base;
    packets.push_back(packet.Ref());
    packet.Unref();
  }
  REQUIRE(result == AVERROR_EOF);
  return packets;
}

void WriteAacWithoutConfig(
    const std::string& path,
    std::size_t audio_limit = std::numeric_limits<std::size_t>::max(),
    bool corrupt_audio = false) {
  const auto input = OpenMedia(SamplePath());
  std::vector<ffmpeg::StreamInfo> streams;
  for (unsigned index = 0; index < input->nb_streams; ++index) {
    const auto* stream = input->streams[index];
    ffmpeg::StreamInfo info;
    info.stream_index = stream->index;
    info.time_base = stream->time_base;
    info.codec_parameters = ffmpeg::CodecParameters(*stream->codecpar);
    auto* parameters = info.codec_parameters.get();
    if (parameters->codec_id == AV_CODEC_ID_AAC) {
      av_freep(&parameters->extradata);
      parameters->extradata_size = 0;
      parameters->profile = AV_PROFILE_UNKNOWN;
    }
    streams.push_back(std::move(info));
  }
  mw::streamer::Recorder recorder;
  mw::streamer::RecorderConfig config;
  config.path = path;
  REQUIRE(recorder.Start(streams, config));
  std::size_t audio_count = 0;
  std::size_t submitted = 0;
  for (auto& packet : ReadPackets(*input)) {
    const auto* parameters = input->streams[packet->stream_index]->codecpar;
    if (parameters->codec_id == AV_CODEC_ID_AAC) {
      if (audio_count == audio_limit) break;
      ++audio_count;
      if (corrupt_audio) {
        REQUIRE(av_packet_make_writable(packet.get()) >= 0);
        std::fill(packet->data, packet->data + packet->size, 0);
      }
    }
    REQUIRE(recorder.TrySubmitPacket(packet));
    ++submitted;
  }
  recorder.Stop();
  REQUIRE(recorder.status().state == RecorderState::kStopped);
  REQUIRE(recorder.status().error.empty());
  REQUIRE(recorder.status().written_packets == submitted);
  REQUIRE(recorder.status().dropped_packets == 0);
}

void CheckOriginalPackets(const std::vector<ffmpeg::Packet>& actual,
                          const std::vector<ffmpeg::Packet>& expected) {
  REQUIRE(actual.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CAPTURE(index);
    const auto& packet = actual[index];
    const auto& original = expected[index];
    CHECK(packet->pts == original->pts);
    CHECK(packet->dts == original->dts);
    CHECK(packet->duration == original->duration);
    CHECK(packet->stream_index == original->stream_index);
    CHECK(packet->time_base.num == original->time_base.num);
    CHECK(packet->time_base.den == original->time_base.den);
    REQUIRE(packet->size == original->size);
    CHECK(std::equal(original->data, original->data + original->size,
                     packet->data));
  }
}

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

std::size_t DecodeCount(const AVStream& stream,
                        const std::vector<ffmpeg::Packet>& packets) {
  const auto* codec = avcodec_find_decoder(stream.codecpar->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecCloser> decoder(
      avcodec_alloc_context3(codec));
  REQUIRE(decoder != nullptr);
  REQUIRE(avcodec_parameters_to_context(decoder.get(), stream.codecpar) >= 0);
  decoder->pkt_timebase = stream.time_base;
  decoder->thread_count = 1;
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  std::size_t count = 0;
  const auto receive = [&] {
    while (true) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      ++count;
    }
  };
  for (const auto& packet : packets) {
    if (packet->stream_index != stream.index) continue;
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
  }
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  return count;
}

void CheckRecording(const std::string& path, const std::string& source_path) {
  const auto source = OpenMedia(source_path);
  const auto recorded = OpenMedia(path);
  const auto original_packets = ReadPackets(*source);
  const auto recorded_packets = ReadPackets(*recorded);
  REQUIRE(recorded->nb_streams == source->nb_streams);
  CHECK(recorded_packets.size() == original_packets.size());
  for (unsigned index = 0; index < source->nb_streams; ++index) {
    const auto* original = source->streams[index];
    const int output_index = av_find_best_stream(
        recorded.get(), original->codecpar->codec_type, -1, -1, nullptr, 0);
    REQUIRE(output_index >= 0);
    const auto* output = recorded->streams[output_index];
    CHECK(output->codecpar->codec_id == original->codecpar->codec_id);
    REQUIRE(original->duration != AV_NOPTS_VALUE);
    REQUIRE(output->duration != AV_NOPTS_VALUE);
    CHECK(std::abs(static_cast<double>(output->duration) *
                       av_q2d(output->time_base) -
                   static_cast<double>(original->duration) *
                       av_q2d(original->time_base)) < 0.1);
    const auto expected = DecodeCount(*original, original_packets);
    REQUIRE(expected > 0);
    CHECK(DecodeCount(*output, recorded_packets) == expected);
  }
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

struct Observations final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ffmpeg::Packet> packets;
  std::vector<std::exception_ptr> errors;

  void Packet(const ffmpeg::Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex);
    packets.push_back(packet.Ref());
    changed.notify_all();
  }
  void Error(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex);
    errors.push_back(error);
    changed.notify_all();
  }
  bool WaitForPackets(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 5s, [&] { return packets.size() >= count; });
  }
};

void Observe(RemuxPipeline& pipeline, Observations& observations) {
  pipeline.SetOnPacket(
      [&](const ffmpeg::Packet& packet) { observations.Packet(packet); });
  pipeline.SetOnError(
      [&](std::exception_ptr error) { observations.Error(error); });
}

}  // namespace

TEST_CASE("RemuxPipeline preserves encoded packets and drains MP4 outputs",
          "[remux_pipeline][integration]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  const auto source = SamplePath();
  const auto input = OpenMedia(source);
  const auto expected = ReadPackets(*input);
  RemuxPipeline pipeline(Config(8.0));
  Observations observations;
  Observe(pipeline, observations);
  const auto first_path = pipeline.AddPushUrl(directory.File("first.mp4"));
  const auto second_path = pipeline.AddPushUrl(directory.File("second.mp4"));
  REQUIRE(pipeline.Start(source));
  CHECK_FALSE(pipeline.Start(source));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kEnded; }));
  pipeline.Stop();
  const auto status = pipeline.status();
  CHECK(status.state == RemuxPipelineState::kEnded);
  CHECK(status.input.state == AvInputProxyState::kEnded);
  CHECK_FALSE(status.error);
  REQUIRE(status.recordings.size() == 2);
  CHECK(observations.errors.empty());
  REQUIRE(observations.packets.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& actual = observations.packets[index];
    const auto& original = expected[index];
    CHECK(actual->pts == original->pts);
    CHECK(actual->dts == original->dts);
    CHECK(actual->duration == original->duration);
    CHECK(actual->stream_index == original->stream_index);
    CHECK(actual->time_base.num == original->time_base.num);
    CHECK(actual->time_base.den == original->time_base.den);
    REQUIRE(actual->size == original->size);
    CHECK(std::equal(original->data, original->data + original->size,
                     actual->data));
  }
  for (const auto& recording : status.recordings) {
    CHECK(recording.status.state == RecorderState::kStopped);
    CHECK(recording.status.error.empty());
    CHECK(recording.status.written_packets == expected.size());
    CHECK(recording.status.dropped_packets == 0);
    CheckRecording(recording.path, source);
  }
  CHECK(fs::exists(fs::u8path(first_path)));
  CHECK(fs::exists(fs::u8path(second_path)));
}

TEST_CASE("RemuxPipeline handles a video-only source without adding tracks",
          "[remux_pipeline][integration]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  const auto source = SamplePath("h264_video.mp4");
  RemuxPipeline pipeline(Config(16.0));
  pipeline.AddPushUrl(directory.File("video.mp4"));
  REQUIRE(pipeline.Start(source));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kEnded; }));
  pipeline.Stop();
  const auto recordings = pipeline.recording_statuses();
  REQUIRE(recordings.size() == 1);
  CheckRecording(recordings.front().path, source);
}

TEST_CASE("AAC sampling timeout interrupts an ordinary blocking input read",
          "[remux_pipeline][aac-probe][timeout]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  mw::streamer::AvInputProxy input;
  REQUIRE(input.Start(directory.File("missing-input.mp4")));
  const auto started = std::chrono::steady_clock::now();
  mw::streamer::internal::AudioProbeTimeout timeout(input);
  auto reader =
      std::async(std::launch::async, [&] { return input.ReadPacket(); });
  const auto ready = reader.wait_for(4s);
  const bool expired = timeout.Finish();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  input.Stop();
  const auto result = reader.get();
  CHECK(ready == std::future_status::ready);
  CHECK(expired);
  CHECK(elapsed >= 1900ms);
  CHECK(elapsed < 4s);
  CHECK(result.status == mw::streamer::AvInputReadStatus::kNull);
  // The expired guard remains alive while the same proxy opens a new session.
  REQUIRE(input.Start(SamplePath()));
  const auto next = input.ReadPacket();
  input.Stop();
  CHECK(next.status == mw::streamer::AvInputReadStatus::kPacket);
}

TEST_CASE("Completed AAC sampling cancels its timer without stopping input",
          "[remux_pipeline][aac-probe][timeout][cancel]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  mw::streamer::AvInputProxy input;
  REQUIRE(input.Start(directory.File("missing-input.mp4")));
  mw::streamer::internal::AudioProbeTimeout timeout(input);
  auto reader =
      std::async(std::launch::async, [&] { return input.ReadPacket(); });
  const auto started = std::chrono::steady_clock::now();
  const bool expired = timeout.Finish();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto pending = reader.wait_for(40ms);
  input.Stop();
  const auto result = reader.get();
  CHECK_FALSE(expired);
  CHECK(elapsed < 500ms);
  CHECK(pending == std::future_status::timeout);
  CHECK(result.status == mw::streamer::AvInputReadStatus::kNull);
}

TEST_CASE("RemuxPipeline probes raw AAC and replays every original packet",
          "[remux_pipeline][integration][aac-probe]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  std::size_t audio_limit = std::numeric_limits<std::size_t>::max();
  SECTION("Normal input exceeds the bounded startup probe") {}
  SECTION("EOF before eight audio packets still drains the startup buffer") {
    audio_limit = 3;
  }
  const auto source = directory.File("without-asc.mp4");
  WriteAacWithoutConfig(source, audio_limit);
  const auto input = OpenMedia(source);
  const int audio_index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  REQUIRE(audio_index >= 0);
  REQUIRE(input->streams[audio_index]->codecpar->extradata_size == 0);
  const auto expected = ReadPackets(*input);
  const auto audio_packets = std::count_if(
      expected.begin(), expected.end(), [&](const ffmpeg::Packet& packet) {
        return packet->stream_index == audio_index;
      });
  if (audio_limit == 3) {
    REQUIRE(audio_packets == 3);
  } else {
    REQUIRE(audio_packets > 8);
  }

  RemuxPipeline pipeline(Config(32.0));
  Observations observations;
  Observe(pipeline, observations);
  const auto output = pipeline.AddPushUrl(directory.File("probed.mp4"));
  REQUIRE(pipeline.Start(source));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kEnded; }));
  pipeline.Stop();
  CHECK_FALSE(pipeline.error());
  CHECK(observations.errors.empty());
  CheckOriginalPackets(observations.packets, expected);
  const auto recordings = pipeline.recording_statuses();
  REQUIRE(recordings.size() == 1);
  CHECK(recordings.front().status.state == RecorderState::kStopped);
  CHECK(recordings.front().status.error.empty());
  CHECK(recordings.front().status.written_packets == expected.size());
  CHECK(recordings.front().status.dropped_packets == 0);

  const auto recorded = OpenMedia(output);
  const int recorded_audio = av_find_best_stream(
      recorded.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  REQUIRE(recorded_audio >= 0);
  CHECK(recorded->streams[recorded_audio]->codecpar->extradata_size > 0);
  CheckRecording(output, source);
}

TEST_CASE(
    "RemuxPipeline rejects undecodable AAC instead of publishing a config",
    "[remux_pipeline][integration][aac-probe][error]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  const auto source = directory.File("corrupt-aac.mp4");
  WriteAacWithoutConfig(source, 12, true);
  RemuxPipeline pipeline(Config(32.0));
  Observations observations;
  Observe(pipeline, observations);
  pipeline.AddPushUrl(directory.File("rejected.mp4"));
  REQUIRE(pipeline.Start(source));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kFailed; }));
  pipeline.Stop();
  REQUIRE(pipeline.error());
  REQUIRE(observations.errors.size() == 1);
  CHECK(observations.errors.front() == pipeline.error());
  CHECK(observations.packets.empty());
}

TEST_CASE("RemuxPipeline paces packet consumption and supports runtime speed",
          "[remux_pipeline][integration][timing]") {
  const auto runtime = InitRuntime();
  const auto run = [&](double speed, bool accelerate) {
    auto config = Config(speed);
    config.max_ahead = 0ms;
    RemuxPipeline pipeline(config);
    Observations observations;
    Observe(pipeline, observations);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(pipeline.Start(SamplePath()));
    REQUIRE(observations.WaitForPackets(8));
    if (accelerate) {
      pipeline.SetSpeed(8.0);
      CHECK(pipeline.speed() == 8.0);
    }
    REQUIRE(WaitFor(
        [&] { return pipeline.state() == RemuxPipelineState::kEnded; }));
    pipeline.Stop();
    CHECK(observations.errors.empty());
    return std::chrono::steady_clock::now() - start;
  };
  const auto normal = run(1.0, false);
  const auto doubled = run(2.0, false);
  const auto changed = run(1.0, true);
  // Bounds allow scheduling noise but reject unpaced reads and ignored speed.
  CHECK(normal > 1300ms);
  CHECK(doubled > 600ms);
  CHECK(doubled < normal * 0.85);
  CHECK(changed < normal * 0.75);
}

TEST_CASE("RemuxPipeline stop wakes a slow pacing wait and works in callbacks",
          "[remux_pipeline][integration][stop]") {
  const auto runtime = InitRuntime();
  auto config = Config(0.001);
  config.max_ahead = 0ms;
  RemuxPipeline pipeline(config);
  Observations observations;
  Observe(pipeline, observations);
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(observations.WaitForPackets(1));
  const auto start = std::chrono::steady_clock::now();
  pipeline.RequestStop();
  pipeline.Stop();
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK(pipeline.state() == RemuxPipelineState::kStopped);
  CHECK(observations.errors.empty());

  std::atomic<int> count{0};
  pipeline.SetOnPacket([&](const ffmpeg::Packet&) {
    (void)pipeline.status();
    ++count;
    pipeline.RequestStop();
  });
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return count.load() == 1; }));
  pipeline.Stop();
  CHECK(count.load() == 1);
  CHECK_FALSE(pipeline.error());
}

TEST_CASE("RemuxPipeline reports terminal input failure exactly once",
          "[remux_pipeline][integration][error]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  RemuxPipeline pipeline(Config());
  Observations observations;
  Observe(pipeline, observations);
  REQUIRE(pipeline.Start(directory.File("missing.mp4")));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kFailed; }));
  pipeline.Stop();
  CHECK(pipeline.state() == RemuxPipelineState::kFailed);
  CHECK(pipeline.error());
  REQUIRE(observations.errors.size() == 1);
  CHECK(observations.errors.front() == pipeline.error());
  CHECK(observations.packets.empty());
}

TEST_CASE("RemuxPipeline isolates recorder file failures from packet delivery",
          "[remux_pipeline][integration][recording]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  RemuxPipeline pipeline(Config(8.0));
  Observations observations;
  Observe(pipeline, observations);
  pipeline.AddPushUrl(directory.File("missing/record.mp4"));
  pipeline.AddPushUrl(directory.File("healthy.mp4"));
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(
      WaitFor([&] { return pipeline.state() == RemuxPipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(observations.errors.empty());
  CHECK_FALSE(pipeline.error());
  const auto recordings = pipeline.recording_statuses();
  REQUIRE(recordings.size() == 2);
  CHECK(recordings[0].status.state == RecorderState::kFailed);
  CHECK_FALSE(recordings[0].status.error.empty());
  CHECK(recordings[1].status.state == RecorderState::kStopped);
  CheckRecording(recordings[1].path, SamplePath());
}

TEST_CASE("RemuxPipeline loops into distinct recordings and retains snapshots",
          "[remux_pipeline][integration][loop]") {
  const auto runtime = InitRuntime();
  const OutputDirectory directory;
  auto config = Config(8.0);
  config.input.loop = true;
  RemuxPipeline pipeline(config);
  Observations observations;
  Observe(pipeline, observations);
  pipeline.AddPushUrl(directory.File("loop.mp4"));
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return pipeline.recording_statuses().size() >= 2; }));
  pipeline.Stop();
  CHECK(pipeline.state() == RemuxPipelineState::kStopped);
  CHECK(observations.errors.empty());
  const auto recordings = pipeline.recording_statuses();
  REQUIRE(recordings.size() >= 2);
  CHECK(recordings[0].path != recordings[1].path);
  CHECK(recordings[0].status.state == RecorderState::kStopped);
  CheckRecording(recordings[0].path, SamplePath());
  pipeline.Stop();
  CHECK(pipeline.recording_statuses().size() == recordings.size());
}

TEST_CASE("RemuxPipeline rejects invalid pacing parameters",
          "[remux_pipeline][configuration]") {
  for (const double speed :
       {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    CHECK_THROWS_AS(RemuxPipeline(Config(speed)), std::invalid_argument);
  }
  RemuxPipeline pipeline;
  for (const double speed :
       {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    CHECK_THROWS_AS(pipeline.SetSpeed(speed), std::invalid_argument);
    CHECK(pipeline.speed() == 1.0);
  }
  pipeline.SetSpeed(2.0);
  CHECK(pipeline.speed() == 2.0);
  auto config = Config();
  config.max_ahead = -1ms;
  CHECK_THROWS_AS(RemuxPipeline(config), std::invalid_argument);
}

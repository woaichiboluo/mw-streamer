#include "mw/streamer/pipeline/live_pipeline.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvMediaSourceState;
using mw::streamer::LivePipeline;
using mw::streamer::LivePipelineConfig;
using mw::streamer::LivePipelineState;
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

std::string SamplePath() {
  return (fs::u8path(MW_STREAMER_TEST_DATA_DIR) / "h264_aac.mp4")
      .generic_u8string();
}

LivePipelineConfig Config() {
  LivePipelineConfig config;
  config.input.proxy.max_retries = 0;
  config.input.proxy.input.packet_capacity = 2;
  config.input.video_decoder.thread_count = 1;
  config.input.audio_decoder.thread_count = 1;
  config.input.video_decoder.frame_capacity = 2;
  config.input.audio_decoder.frame_capacity = 2;
  auto& encoder = config.encoder;
  encoder.width = 64;
  encoder.height = 64;
  encoder.video_pixel_format = AV_PIX_FMT_YUV420P;
  encoder.fps = {25, 1};
  // The output callback exposes packets, not the encoder's global extradata.
  // Keep SPS/PPS in this fixture's bitstream for standalone memory decoding.
  encoder.video_options = {{"preset", "ultrafast"},
                           {"tune", "zerolatency"},
                           {"x264-params", "repeat-headers=1"}};
  encoder.audio_sample_format = AV_SAMPLE_FMT_FLTP;
  encoder.audio_sample_rate = 48000;
  config.syncer.timeout = 20ms;
  config.syncer.freeze = 20ms;
  config.syncer.start_ts = 0us;
  config.syncer.forward_start_shift = true;
  return config;
}

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(fs::temp_directory_path() /
              ("mw-pipeline-" + std::to_string(std::chrono::steady_clock::now()
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
  bool empty() const { return fs::is_empty(path_); }

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
  AVFormatContext* context = nullptr;
  REQUIRE(avformat_open_input(&context, path.c_str(), nullptr, nullptr) >= 0);
  std::unique_ptr<AVFormatContext, FormatCloser> input(context);
  REQUIRE(avformat_find_stream_info(input.get(), nullptr) >= 0);
  return input;
}

struct Observations final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ffmpeg::Packet> input;
  std::vector<ffmpeg::Packet> output;
  std::vector<std::exception_ptr> errors;

  void Input(const ffmpeg::Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex);
    input.push_back(packet.Ref());
    changed.notify_all();
  }
  void Output(const ffmpeg::Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex);
    output.push_back(packet.Ref());
    changed.notify_all();
  }
  void Error(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex);
    errors.push_back(error);
    changed.notify_all();
  }
  std::size_t ErrorCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return errors.size();
  }
  template <typename Predicate>
  bool Wait(Predicate predicate, std::chrono::milliseconds timeout = 8s) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, predicate);
  }
};

void Observe(LivePipeline& pipeline, Observations& observations) {
  pipeline.SetOnInputPacket(
      [&](const ffmpeg::Packet& packet) { observations.Input(packet); });
  pipeline.SetOnOutputPacket(
      [&](const ffmpeg::Packet& packet) { observations.Output(packet); });
  pipeline.SetOnError(
      [&](std::exception_ptr error) { observations.Error(error); });
}

struct CodecCloser {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

std::vector<ffmpeg::Frame> DecodePackets(
    const std::vector<ffmpeg::Packet>& packets, bool video,
    std::size_t begin = 0) {
  const int stream_index = video ? 0 : 1;
  const auto* codec =
      avcodec_find_decoder(video ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecCloser> decoder(
      avcodec_alloc_context3(codec));
  REQUIRE(decoder != nullptr);
  decoder->thread_count = 1;
  if (!video) {
    // Config() fixes AAC-LC at 48 kHz stereo. Raw AAC packets need the matching
    // AudioSpecificConfig; H264 carries SPS/PPS in its Annex B packets.
    decoder->sample_rate = 48000;
    av_channel_layout_default(&decoder->ch_layout, 2);
    decoder->extradata = static_cast<std::uint8_t*>(
        av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
    REQUIRE(decoder->extradata != nullptr);
    decoder->extradata_size = 2;
    decoder->extradata[0] = 0x11;
    decoder->extradata[1] = 0x90;
  }
  REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  std::vector<ffmpeg::Frame> frames;
  const auto receive = [&] {
    while (true) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      frames.push_back(std::move(frame));
    }
  };
  for (std::size_t index = begin; index < packets.size(); ++index) {
    const auto& packet = packets[index];
    if (packet->stream_index != stream_index) continue;
    decoder->pkt_timebase = packet->time_base;
    REQUIRE(avcodec_send_packet(decoder.get(), packet.get()) >= 0);
    receive();
  }
  REQUIRE(avcodec_send_packet(decoder.get(), nullptr) >= 0);
  receive();
  return frames;
}

void CheckEncoded(const std::vector<ffmpeg::Packet>& packets, bool video,
                  bool audio, std::size_t begin = 0) {
  REQUIRE(begin < packets.size());
  std::size_t video_packets = 0;
  std::size_t audio_packets = 0;
  for (std::size_t index = begin; index < packets.size(); ++index) {
    const auto& packet = packets[index];
    CHECK(packet->size > 0);
    CHECK(packet->time_base.num > 0);
    CHECK(packet->time_base.den > 0);
    CHECK(packet->pts != AV_NOPTS_VALUE);
    CHECK(packet->stream_index >= 0);
    CHECK(packet->stream_index <= 1);
    if (packet->stream_index == 0) ++video_packets;
    if (packet->stream_index == 1) ++audio_packets;
  }
  CHECK((video_packets > 0) == video);
  CHECK((audio_packets > 0) == audio);
  if (video) {
    const auto frames = DecodePackets(packets, true, begin);
    REQUIRE_FALSE(frames.empty());
    for (const auto& frame : frames) {
      CHECK(frame->format == AV_PIX_FMT_YUV420P);
      CHECK(frame->width == 64);
      CHECK(frame->height == 64);
    }
  }
  if (audio) {
    const auto frames = DecodePackets(packets, false, begin);
    REQUIRE_FALSE(frames.empty());
    for (const auto& frame : frames) {
      CHECK(frame->sample_rate == 48000);
      CHECK(frame->ch_layout.nb_channels == 2);
      CHECK(frame->nb_samples > 0);
    }
  }
}

void CheckRecording(const std::string& path, bool video, bool audio,
                    bool decode = true) {
  REQUIRE(fs::exists(fs::u8path(path)));
  const auto input = OpenMedia(path);
  CHECK((av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr,
                             0) >= 0) == video);
  CHECK((av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr,
                             0) >= 0) == audio);
  if (!decode) return;
  std::vector<std::unique_ptr<AVCodecContext, CodecCloser>> decoders;
  std::vector<std::size_t> frames(input->nb_streams, 0);
  for (unsigned int index = 0; index < input->nb_streams; ++index) {
    const auto* parameters = input->streams[index]->codecpar;
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    REQUIRE(codec != nullptr);
    auto& decoder = decoders.emplace_back(avcodec_alloc_context3(codec));
    REQUIRE(decoder != nullptr);
    REQUIRE(avcodec_parameters_to_context(decoder.get(), parameters) >= 0);
    decoder->thread_count = 1;
    REQUIRE(avcodec_open2(decoder.get(), codec, nullptr) >= 0);
  }
  const auto receive = [&](std::size_t index) {
    for (;;) {
      ffmpeg::Frame frame;
      const int result =
          avcodec_receive_frame(decoders[index].get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      REQUIRE(result >= 0);
      ++frames[index];
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    REQUIRE(result >= 0);
    const auto index = static_cast<std::size_t>(packet->stream_index);
    REQUIRE(index < decoders.size());
    REQUIRE(avcodec_send_packet(decoders[index].get(), packet.get()) >= 0);
    receive(index);
  }
  for (std::size_t index = 0; index < decoders.size(); ++index) {
    REQUIRE(avcodec_send_packet(decoders[index].get(), nullptr) >= 0);
    receive(index);
    CHECK(frames[index] > 0);
  }
}

TEST_CASE("LivePipeline delivers decodable AV and continues after input EOF",
          "[pipeline][integration][eof]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Observations observations;
  LivePipeline pipeline(Config());
  Observe(pipeline, observations);
  CHECK(pipeline.state() == LivePipelineState::kStopped);
  const auto output = pipeline.AddPushUrl(directory.File("live.mp4"));
  REQUIRE(pipeline.Start(SamplePath()));
  CHECK_FALSE(pipeline.Start(SamplePath()));
  REQUIRE(observations.Wait([&] {
    return pipeline.input_status().state == AvMediaSourceState::kEnded ||
           !observations.errors.empty();
  }));
  REQUIRE(observations.ErrorCount() == 0);
  REQUIRE(pipeline.input_status().state == AvMediaSourceState::kEnded);
  std::size_t before = 0;
  {
    std::lock_guard<std::mutex> lock(observations.mutex);
    before = observations.output.size();
  }
  REQUIRE(observations.Wait(
      [&] { return observations.output.size() >= before + 8; }));
  const auto snapshot = pipeline.status();
  CHECK(snapshot.state == LivePipelineState::kRunning);
  CHECK(snapshot.input.state == AvMediaSourceState::kEnded);
  CHECK(snapshot.remuxer.empty());
  CHECK(pipeline.remuxer_status().empty());
  CHECK(snapshot.error == nullptr);
  const std::string target = "rtsp://127.0.0.1:1/live_pipeline/unavailable";
  CHECK(pipeline.AddPushUrl(target) == target);
  // Leaving recording-only mode needs subsequent frames to register the
  // protocol source. Pending targets are excluded from pusher snapshots.
  REQUIRE(
      observations.Wait([&] { return !pipeline.remuxer_status().empty(); }));
  const auto pushes = pipeline.remuxer_status();
  REQUIRE(pushes.size() == 1);
  CHECK(pushes.front().url == target);
  CHECK(pushes.front().status == 1);
  REQUIRE(pipeline.status().remuxer.size() == 1);
  pipeline.Stop();
  CHECK(pipeline.state() == LivePipelineState::kStopped);
  REQUIRE(observations.errors.empty());
  REQUIRE_FALSE(observations.input.empty());
  REQUIRE_FALSE(observations.output.empty());

  // Every observer sees the native input header, before conversion/encoding.
  const auto native = OpenMedia(SamplePath());
  ffmpeg::Packet packet;
  std::size_t index = 0;
  while (av_read_frame(native.get(), packet.get()) >= 0) {
    REQUIRE(index < observations.input.size());
    const auto& observed = observations.input[index++];
    CHECK(observed->stream_index == packet->stream_index);
    CHECK(observed->pts == packet->pts);
    CHECK(observed->dts == packet->dts);
    CHECK(observed->size == packet->size);
    packet.Unref();
  }
  CHECK(index == observations.input.size());
  CheckEncoded(observations.output, true, true);
  CheckRecording(output, true, true);
}

TEST_CASE("LivePipeline records local MP4 before and during a session",
          "[pipeline][integration][local-output]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Observations observations;
  LivePipeline pipeline(Config());
  Observe(pipeline, observations);
  const auto before = pipeline.AddPushUrl(directory.File("before.mp4"));
  REQUIRE_THROWS_AS(pipeline.AddPushUrl(directory.File("before.m3u8")),
                    std::invalid_argument);
  CHECK(directory.empty());
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(observations.Wait([&] {
    const auto video_packets = std::count_if(
        observations.output.begin(), observations.output.end(),
        [](const auto& packet) { return packet->stream_index == 0; });
    const auto audio_packets = std::count_if(
        observations.output.begin(), observations.output.end(),
        [](const auto& packet) { return packet->stream_index == 1; });
    return (video_packets >= 2 && audio_packets >= 2) ||
           !observations.errors.empty();
  }));
  const auto during = pipeline.AddPushUrl(directory.File("during.mp4"));
  REQUIRE_THROWS_AS(pipeline.AddPushUrl(directory.File("during.m3u8")),
                    std::invalid_argument);
  std::size_t previous = 0;
  {
    std::lock_guard<std::mutex> lock(observations.mutex);
    previous = observations.output.size();
  }
  REQUIRE(observations.Wait([&] {
    return observations.output.size() >= previous + 8 ||
           !observations.errors.empty();
  }));
  CHECK(pipeline.remuxer_status().empty());
  CHECK(pipeline.state() == LivePipelineState::kRunning);
  pipeline.Stop();
  REQUIRE(observations.errors.empty());
  CheckEncoded(observations.output, true, true);
  CheckRecording(before, true, true);
  // Dynamic registration keeps subsequent packets without keyframe selection.
  CheckRecording(during, true, true, false);
}

TEST_CASE("LivePipeline continues encoding after local recording fails",
          "[pipeline][integration][local-output][error]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  const auto parent_file = directory.File("not-a-directory");
  {
    std::ofstream file(parent_file);
    REQUIRE(file.is_open());
  }
  Observations observations;
  LivePipeline pipeline(Config());
  Observe(pipeline, observations);
  pipeline.AddPushUrl(parent_file + "/failed.mp4");
  REQUIRE(pipeline.Start(SamplePath()));
  REQUIRE(observations.Wait([&] {
    return observations.output.size() >= 20 || !observations.errors.empty();
  }));
  CHECK(pipeline.state() == LivePipelineState::kRunning);
  CHECK(pipeline.error() == nullptr);
  pipeline.Stop();
  REQUIRE(observations.errors.empty());
  CheckEncoded(observations.output, true, true);
}

TEST_CASE("LivePipeline retains and encodes the top-level standby frame",
          "[pipeline][integration][backup]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Observations observations;
  auto config = Config();
  config.input.proxy.max_retries = -1;
  config.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  ffmpeg::Frame backup;
  backup->format = AV_PIX_FMT_YUV420P;
  backup->width = 64;
  backup->height = 64;
  REQUIRE(av_frame_get_buffer(backup.get(), 0) >= 0);
  for (int plane = 0; plane < 3; ++plane) {
    const int dimension = plane == 0 ? 64 : 32;
    for (int row = 0; row < dimension; ++row) {
      std::fill_n(backup->data[plane] + row * backup->linesize[plane],
                  dimension, plane == 0 ? 180 : 128);
    }
  }
  config.backup_video = backup.Ref();
  LivePipeline pipeline(config);
  // The pipeline owns its headers and buffer references independently.
  config.backup_video.reset();
  backup.Unref();
  Observe(pipeline, observations);
  const auto output = pipeline.AddPushUrl(directory.File("standby.mp4"));
  REQUIRE(pipeline.Start(directory.File("missing.mp4")));
  REQUIRE(observations.Wait(
      [&] {
        return observations.output.size() >= 6 || !observations.errors.empty();
      },
      4s));
  pipeline.Stop();
  REQUIRE(observations.errors.empty());
  CHECK(pipeline.error() == nullptr);
  CHECK(observations.input.empty());
  REQUIRE(observations.output.size() >= 6);

  CheckRecording(output, true, false);
  const auto frames = DecodePackets(observations.output, true);
  REQUIRE_FALSE(frames.empty());
  const auto& decoded = frames.front();
  REQUIRE(decoded->format == AV_PIX_FMT_YUV420P);
  REQUIRE(decoded->width == 64);
  REQUIRE(decoded->height == 64);
  for (int row = 0; row < decoded->height; ++row) {
    for (int column = 0; column < decoded->width; ++column) {
      const int luma = decoded->data[0][row * decoded->linesize[0] + column];
      REQUIRE(luma >= 178);
      REQUIRE(luma <= 182);
    }
  }
}

TEST_CASE("LivePipeline supports single-track output and restart",
          "[pipeline][integration][restart]") {
  const auto runtime = InitRuntime();
  auto config = Config();
  bool video = true;
  SECTION("video only") {
    config.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  }
  SECTION("audio only") {
    video = false;
    config.encoder.video_pixel_format = AV_PIX_FMT_NONE;
  }
  OutputDirectory directory;
  Observations observations;
  LivePipeline pipeline(config);
  Observe(pipeline, observations);
  for (int session = 0; session < 2; ++session) {
    const auto output = pipeline.AddPushUrl(
        directory.File("session-" + std::to_string(session) + ".mp4"));
    std::size_t before = 0;
    {
      std::lock_guard<std::mutex> lock(observations.mutex);
      before = observations.output.size();
    }
    REQUIRE(pipeline.Start(SamplePath()));
    REQUIRE(observations.Wait([&] {
      return observations.output.size() >= before + 6 ||
             !observations.errors.empty();
    }));
    REQUIRE_THROWS_AS(pipeline.SetOnInputPacket({}), std::logic_error);
    REQUIRE_THROWS_AS(pipeline.SetOnOutputPacket({}), std::logic_error);
    REQUIRE_THROWS_AS(pipeline.SetOnError({}), std::logic_error);
    REQUIRE_THROWS_AS(pipeline.media_filter(), std::logic_error);
    pipeline.RequestStop();
    pipeline.Stop();
    pipeline.Stop();
    REQUIRE(observations.errors.empty());
    CHECK(pipeline.error() == nullptr);
    CheckEncoded(observations.output, video, !video, before);
    CheckRecording(output, video, !video);
    // Completed Stop provides a callback barrier; retained packets stay valid.
    const auto count = observations.output.size();
    pipeline.Stop();
    CHECK(observations.output.size() == count);
  }
}

class SessionError final : public std::runtime_error {
 public:
  SessionError() : std::runtime_error("pipeline callback failure") {}
};

TEST_CASE(
    "LivePipeline reports the first component failure and permits snapshots",
    "[pipeline][error]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Observations observations;
  LivePipeline pipeline(Config());
  std::string input = SamplePath();
  bool callback_error = true;
  SECTION("source terminal failure") {
    callback_error = false;
    input = directory.File("missing.mp4");
  }
  SECTION("scheduler business filter failure") {
    pipeline.media_filter().SetOnVideo(
        [](const ffmpeg::Frame&) -> ffmpeg::Frame { throw SessionError(); });
  }
  SECTION("input observer failure") {
    pipeline.SetOnInputPacket(
        [](const ffmpeg::Packet&) { throw SessionError(); });
  }
  SECTION("encoded output observer failure") {
    pipeline.SetOnOutputPacket(
        [](const ffmpeg::Packet&) { throw SessionError(); });
  }
  bool snapshot_failed = false;
  pipeline.SetOnError([&](std::exception_ptr error) {
    const auto snapshot = pipeline.status();
    snapshot_failed =
        snapshot.state == LivePipelineState::kFailed && snapshot.error == error;
    pipeline.RequestStop();
    observations.Error(error);
  });
  const bool started = pipeline.Start(input);
  REQUIRE(observations.Wait([&] { return !observations.errors.empty(); }));
  pipeline.Stop();
  CHECK(snapshot_failed);
  CHECK(pipeline.state() == LivePipelineState::kFailed);
  CHECK(observations.errors.size() == 1);
  REQUIRE(pipeline.error() != nullptr);
  CHECK(pipeline.error() == observations.errors.front());
  if (callback_error) {
    CHECK_THROWS_AS(std::rethrow_exception(pipeline.error()), SessionError);
  }
  static_cast<void>(started);
}

TEST_CASE("LivePipeline rolls back startup failure before restarting",
          "[pipeline][startup][restart]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  Observations observations;
  LivePipeline pipeline(Config());
  Observe(pipeline, observations);
  pipeline.media_filter().SetOnStart([] { throw SessionError(); });
  pipeline.AddPushUrl(directory.File("failed.mp4"));
  CHECK_FALSE(pipeline.Start(SamplePath()));
  CHECK(pipeline.state() == LivePipelineState::kFailed);
  REQUIRE(observations.errors.size() == 1);
  CHECK_THROWS_AS(std::rethrow_exception(pipeline.error()), SessionError);
  pipeline.Stop();
  pipeline.media_filter().SetOnStart({});
  const auto output = pipeline.AddPushUrl(directory.File("recovered.mp4"));
  REQUIRE(pipeline.Start(SamplePath()));
  CHECK(pipeline.error() == nullptr);
  REQUIRE(observations.Wait([&] { return observations.output.size() >= 10; }));
  pipeline.Stop();
  CHECK(observations.errors.size() == 1);
  CHECK(pipeline.state() == LivePipelineState::kStopped);
  CheckEncoded(observations.output, true, true);
  CheckRecording(output, true, true);
}

TEST_CASE("LivePipeline honors RequestStop during startup without an error",
          "[pipeline][startup][cancel]") {
  const auto runtime = InitRuntime();
  Observations observations;
  LivePipeline pipeline(Config());
  Observe(pipeline, observations);
  pipeline.media_filter().SetOnStart([&] { pipeline.RequestStop(); });
  CHECK_FALSE(pipeline.Start(SamplePath()));
  CHECK(pipeline.state() == LivePipelineState::kStopped);
  CHECK(pipeline.error() == nullptr);
  pipeline.Stop();
  CHECK(observations.errors.empty());
  CHECK(observations.output.empty());
  const auto input_count = observations.input.size();
  pipeline.Stop();
  CHECK(observations.input.size() == input_count);
}

}  // namespace

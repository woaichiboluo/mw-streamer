#include "mw/streamer/pipeline/live_pipeline.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "multitrack_fixture.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::InputMode;
using mw::streamer::LivePipeline;
using mw::streamer::LivePipelineConfig;
using mw::streamer::PipelineState;
namespace ffmpeg = mw::streamer::ffmpeg;

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(
            std::filesystem::current_path() /
            ("live-pipeline-" + std::to_string(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count()))) {
    std::filesystem::create_directories(path_);
  }
  ~OutputDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  std::string File(const std::string& name = "record.mp4") const {
    return (path_ / name).generic_u8string();
  }

 private:
  std::filesystem::path path_;
};

std::string Fixture(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) + "/" + name;
}

LivePipelineConfig Config() {
  LivePipelineConfig config;
  config.input.auto_reconnect = false;
  config.encoder.width = 64;
  config.encoder.height = 64;
  config.encoder.video_pixel_format = AV_PIX_FMT_YUV420P;
  config.encoder.video_options = {{"preset", "ultrafast"}};
  // Preserve codec delay so EOF and active Stop must drain it.
  config.encoder.max_b_frames = 2;
  return config;
}

bool WaitUntil(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

class CallbackGate final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [this] { return entered_; });
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool released_ = false;
};

struct ReleaseGate final {
  CallbackGate& gate;
  ~ReleaseGate() { gate.Release(); }
};

struct StopPipeline final {
  LivePipeline& pipeline;
  ~StopPipeline() { pipeline.Stop(); }
};

struct FormatCloser final {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

std::unique_ptr<AVFormatContext, FormatCloser> OpenMedia(
    const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开 LivePipeline 测试媒体");
  std::unique_ptr<AVFormatContext, FormatCloser> context(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "探测 LivePipeline 测试媒体");
  return context;
}

// Derive single-track fixtures from the same source without an external tool.
void ExtractTrack(const std::string& output, AVMediaType type) {
  auto input = OpenMedia(Fixture());
  const int index = av_find_best_stream(input.get(), type, -1, -1, nullptr, 0);
  ffmpeg::FfmpegException::throwIfError(index, "选择单轨测试媒体");
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_alloc_output_context2(&raw, nullptr, "mp4", output.c_str()),
      "创建单轨测试媒体");
  const auto close = [](AVFormatContext* context) {
    if (context->pb) avio_closep(&context->pb);
    avformat_free_context(context);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  auto* stream = avformat_new_stream(raw, nullptr);
  if (!stream) throw std::bad_alloc();
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_copy(stream->codecpar,
                              input->streams[index]->codecpar),
      "复制单轨测试描述");
  stream->codecpar->codec_tag = 0;
  stream->time_base = input->streams[index]->time_base;
  ffmpeg::FfmpegException::throwIfError(
      avio_open(&raw->pb, output.c_str(), AVIO_FLAG_WRITE), "打开单轨测试输出");
  ffmpeg::FfmpegException::throwIfError(avformat_write_header(raw, nullptr),
                                        "写入单轨测试头");
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(input.get(), packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "读取单轨测试包");
    if (packet->stream_index != index) continue;
    av_packet_rescale_ts(packet.get(), input->streams[index]->time_base,
                         stream->time_base);
    packet->stream_index = 0;
    ffmpeg::FfmpegException::throwIfError(
        av_interleaved_write_frame(raw, packet.get()), "写入单轨测试包");
  }
  ffmpeg::FfmpegException::throwIfError(av_write_trailer(raw),
                                        "完成单轨测试媒体");
}

struct DecodedTrack final {
  int frames = 0;
  int width = 0;
  int height = 0;
  int sample_rate = 0;
  std::int64_t samples = 0;
  double first = std::numeric_limits<double>::infinity();
  double end = 0;
};

// Decode every packet and codec tail, checking container order independently
// of pipeline callback counters and retained ready snapshots.
std::map<AVMediaType, DecodedTrack> DecodeRecording(const std::string& path) {
  auto format = OpenMedia(path);
  std::map<int, ffmpeg::CodecContext> decoders;
  std::map<int, std::int64_t> previous_dts;
  std::map<AVMediaType, DecodedTrack> result;
  for (unsigned int index = 0; index < format->nb_streams; ++index) {
    auto* stream = format->streams[index];
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
      continue;
    ffmpeg::CodecContext decoder(
        avcodec_find_decoder(stream->codecpar->codec_id));
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(decoder.get(), stream->codecpar),
        "配置录像解码器");
    decoder.get()->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr), "打开录像解码器");
    decoders.emplace(static_cast<int>(index), std::move(decoder));
  }
  const auto receive = [&](int index, ffmpeg::CodecContext& decoder) {
    ffmpeg::Frame frame;
    for (;;) {
      const int status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(status, "解码 LivePipeline 录像");
      auto& track = result[decoder.get()->codec_type];
      ++track.frames;
      track.width = frame->width;
      track.height = frame->height;
      track.sample_rate = frame->sample_rate;
      track.samples += frame->nb_samples;
      REQUIRE(frame->best_effort_timestamp != AV_NOPTS_VALUE);
      const double pts = static_cast<double>(frame->best_effort_timestamp) *
                         av_q2d(format->streams[index]->time_base);
      track.first = std::min(track.first, pts);
      const double duration =
          frame->nb_samples
              ? static_cast<double>(frame->nb_samples) / frame->sample_rate
              : 1.0 / 30.0;
      track.end = std::max(track.end, pts + duration);
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(format.get(), packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "读取 LivePipeline 录像");
    const auto decoder = decoders.find(packet->stream_index);
    if (decoder == decoders.end()) continue;
    REQUIRE(packet->dts != AV_NOPTS_VALUE);
    if (previous_dts.count(packet->stream_index))
      CHECK(packet->dts > previous_dts.at(packet->stream_index));
    previous_dts[packet->stream_index] = packet->dts;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder->second.get(), packet.get()),
        "提交录像解码包");
    receive(packet->stream_index, decoder->second);
  }
  for (auto& [index, decoder] : decoders) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder.get(), nullptr), "排空录像解码器");
    receive(index, decoder);
  }
  return result;
}

TEST_CASE("LivePipeline rejects incompatible input and frame rates",
          "[pipeline][live][configuration]") {
  Runtime runtime;
  auto config = Config();
  CHECK(config.input.mode == InputMode::kLive);
  SECTION("Other input modes cannot assemble a live chain") {
    config.input.mode = GENERATE(InputMode::kBatch, InputMode::kRemux);
    CHECK_THROWS_AS(LivePipeline{config}, std::invalid_argument);
  }
  SECTION("Scheduling and encoding must use the same frame rate") {
    config.encoder.fps = {25, 1};
    CHECK_THROWS_AS(LivePipeline{config}, std::invalid_argument);
  }
  SECTION("Equivalent rational rates are accepted") {
    config.encoder.fps = {60, 2};
    CHECK_NOTHROW(LivePipeline{config});
  }
  LivePipeline pipeline(Config());
  CHECK(pipeline.state() == PipelineState::kIdle);
  CHECK_THROWS_AS(pipeline.Start(""), std::invalid_argument);
}

TEST_CASE("LivePipeline drains encoded audio video and preserves EOF order",
          "[pipeline][live][media][eof]") {
  Runtime runtime;
  OutputDirectory directory;
  std::atomic<int> ready{0}, videos{0}, audios{0}, invalid{0};
  std::atomic<int> sequence{0}, ended{0}, stopped{0}, terminal{0};
  std::thread::id video_worker, audio_worker;
  LivePipeline pipeline(Config());
  const auto output = pipeline.AddPushUrl(directory.File());
  pipeline.SetOnReady([&](const auto& streams, const auto& device) noexcept {
    ++ready;
    if (streams.size() != 2 || device.get() != nullptr) ++invalid;
    for (const auto& stream : streams) {
      if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO &&
          (stream.codec_parameters.get()->sample_rate != 48000 ||
           stream.codec_parameters.get()->format != AV_SAMPLE_FMT_S32))
        ++invalid;
    }
    return true;
  });
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    const int position = videos++;
    video_worker = std::this_thread::get_id();
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width != 64 ||
        frame->height != 64 || frame->pts != position || frame->duration != 1 ||
        av_cmp_q(frame->time_base, AVRational{1, 30}) != 0)
      ++invalid;
    return frame.Ref();
  });
  pipeline.SetOnAudio([&](const auto& frame) noexcept {
    const int position = audios++;
    audio_worker = std::this_thread::get_id();
    if (frame->format != AV_SAMPLE_FMT_S32 || frame->sample_rate != 48000 ||
        frame->nb_samples != 1600 || frame->pts != position * 1600 ||
        frame->duration != 1600 ||
        av_cmp_q(frame->time_base, AVRational{1, 48000}) != 0)
      ++invalid;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ended = ++sequence; });
  pipeline.SetOnStop([&]() noexcept { stopped = ++sequence; });
  pipeline.SetOnStateChanged([&](const auto& status) {
    if (status.state == PipelineState::kEnded) terminal = ++sequence;
  });
  pipeline.Start(Fixture());
  CHECK(ready == 1);
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  REQUIRE(info.scheduled_streams.size() == 2);
  REQUIRE(info.encoded_streams.size() == 2);
  CHECK(info.output_urls == std::vector<std::string>{output});
  for (const auto& stream : info.streams) {
    const auto* parameters = stream.codec_parameters.get();
    if (parameters->codec_type == AVMEDIA_TYPE_AUDIO)
      CHECK(parameters->sample_rate == 48000);
  }
  for (const auto& stream : info.scheduled_streams) {
    const auto* parameters = stream.codec_parameters.get();
    if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      CHECK(parameters->sample_rate == 48000);
      CHECK(parameters->format == AV_SAMPLE_FMT_S32);
      CHECK(av_cmp_q(stream.time_base, AVRational{1, 48000}) == 0);
    } else {
      CHECK(av_cmp_q(stream.time_base, AVRational{1, 30}) == 0);
    }
  }
  for (const auto& stream : info.encoded_streams) {
    const auto* parameters = stream.codec_parameters.get();
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(parameters->codec_id == AV_CODEC_ID_H264);
      CHECK(parameters->width == 64);
      CHECK(parameters->height == 64);
    } else {
      CHECK(parameters->codec_id == AV_CODEC_ID_AAC);
      CHECK(parameters->sample_rate == 48000);
    }
  }
  REQUIRE(WaitUntil([&] { return terminal != 0; }));
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kEnded);
  CHECK(invalid == 0);
  CHECK(video_worker == audio_worker);
  CHECK(ended == 1);
  CHECK(stopped == 2);
  CHECK(terminal == 3);
  const auto media = DecodeRecording(output);
  REQUIRE(media.size() == 2);
  const auto& video = media.at(AVMEDIA_TYPE_VIDEO);
  const auto& audio = media.at(AVMEDIA_TYPE_AUDIO);
  CHECK(video.frames == videos);
  CHECK(video.frames >= 55);
  CHECK(video.frames <= 65);
  CHECK(video.width == 64);
  CHECK(video.height == 64);
  CHECK(audio.frames > 0);
  CHECK(audios > 0);
  CHECK(audio.sample_rate == 48000);
  CHECK(audio.samples >= 85000);
  CHECK(std::abs(video.first - audio.first) < 0.15);
  CHECK(std::abs(video.end - audio.end) < 0.15);
}

TEST_CASE("LivePipeline handles single track files without absent-track waits",
          "[pipeline][live][single-track]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto type = GENERATE(AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO);
  const auto source = directory.File("input.mp4");
  ExtractTrack(source, type);
  auto config = Config();
  config.encoder.width = config.encoder.height = 0;
  LivePipeline pipeline(config);
  const auto output = pipeline.AddPushUrl(directory.File());
  pipeline.Start(source);
  REQUIRE(pipeline.ready_info().encoded_streams.size() == 1);
  CHECK(pipeline.ready_info()
            .encoded_streams.front()
            .codec_parameters.get()
            ->codec_type == type);
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  const auto media = DecodeRecording(output);
  REQUIRE(media.size() == 1);
  CHECK(media.at(type).frames > 0);
  CHECK(media.at(type).end > 1.8);
}

TEST_CASE(
    "LivePipeline encodes fractional channel cadence without sample drift",
    "[pipeline][live][media][cadence][eof]") {
  Runtime runtime;
  OutputDirectory directory;
  const int numerator = GENERATE(30000, 60000);
  const AVRational rate{numerator, 1001};
  auto config = Config();
  config.scheduler.video_frame_rate = rate;
  config.encoder.fps = rate;
  LivePipeline pipeline(config);
  const auto output = pipeline.AddPushUrl(directory.File());
  std::atomic<int> videos{0}, audios{0}, invalid{0}, ended{0}, stopped{0};
  std::int64_t total_samples = 0;
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    const auto tick = videos++;
    if (frame->pts != tick || frame->duration != 1 ||
        av_cmp_q(frame->time_base, av_inv_q(rate)) != 0)
      ++invalid;
    return frame.Ref();
  });
  pipeline.SetOnAudio([&](const auto& frame) noexcept {
    constexpr int p2997[] = {1602, 1601, 1602, 1601, 1602};
    constexpr int p5994[] = {801, 800, 801, 801, 801};
    const auto tick = audios++;
    const int expected =
        numerator == 30000 ? p2997[(tick + 1) % 5] : p5994[(tick + 1) % 5];
    if (frame->pts != total_samples || frame->nb_samples != expected ||
        frame->duration != expected || frame->sample_rate != 48000 ||
        av_cmp_q(frame->time_base, AVRational{1, 48000}) != 0)
      ++invalid;
    total_samples += frame->nb_samples;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  StopPipeline stop_on_failure{pipeline};
  pipeline.Start(Fixture());
  REQUIRE(WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(invalid == 0);
  CHECK(videos == audios);
  CHECK(ended == 1);
  CHECK(stopped == 1);
  const auto media = DecodeRecording(output);
  const auto& video = media.at(AVMEDIA_TYPE_VIDEO);
  const auto& audio = media.at(AVMEDIA_TYPE_AUDIO);
  CHECK(video.frames == videos);
  // AAC may expose priming/tail padding when decoded, bounded by one packet
  // at each end. This independently checks that native sample counts were
  // not rescaled through a second clock or compressed after encoding.
  CHECK(std::abs(audio.samples - total_samples) <= 2048);
  CHECK(std::abs(video.first - audio.first) < 0.15);
  CHECK(std::abs(video.end - audio.end) < 0.15);
}

TEST_CASE("LivePipeline Start waits for processed output and Stop joins it",
          "[pipeline][live][startup][control]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = directory.File("input.mp4");
  ExtractTrack(source, AVMEDIA_TYPE_VIDEO);
  CallbackGate gate;
  std::atomic<int> frames{0}, ended{0}, stopped{0};
  auto config = Config();
  config.input.loop = true;
  LivePipeline pipeline(config);
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    if (++frames == 1) gate.Enter();
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  ReleaseGate release_pipeline{gate};
  auto start = std::async(std::launch::async, [&] { pipeline.Start(source); });
  ReleaseGate release_start{gate};
  REQUIRE(gate.Wait());
  CHECK(start.wait_for(100ms) == std::future_status::timeout);
  CHECK(pipeline.state() == PipelineState::kStarting);
  gate.Release();
  REQUIRE(start.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(start.get());
  CHECK(pipeline.state() == PipelineState::kRunning);
  pipeline.Stop();
  CHECK(ended == 0);
  CHECK(stopped == 1);
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("LivePipeline active Stop waits for Processor and drains Encoder",
          "[pipeline][live][control][recording]") {
  Runtime runtime;
  OutputDirectory directory;
  CallbackGate gate;
  std::atomic<int> frames{0}, ended{0}, stopped{0};
  auto config = Config();
  config.input.loop = true;
  LivePipeline pipeline(config);
  const auto output = pipeline.AddPushUrl(directory.File());
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    if (++frames == 12) gate.Enter();
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  ReleaseGate release_pipeline{gate};
  pipeline.Start(Fixture());
  REQUIRE(gate.Wait());
  auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
  ReleaseGate release_stop{gate};
  REQUIRE(
      WaitUntil([&] { return pipeline.state() == PipelineState::kStopping; }));
  CHECK(stop.wait_for(100ms) == std::future_status::timeout);
  CHECK(stopped == 0);
  gate.Release();
  REQUIRE(stop.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(stop.get());
  const int final_frames = frames;
  pipeline.Stop();
  std::this_thread::sleep_for(30ms);
  CHECK(frames == final_frames);
  CHECK(ended == 0);
  CHECK(stopped == 1);
  const auto media = DecodeRecording(output);
  CHECK(media.at(AVMEDIA_TYPE_VIDEO).frames == final_frames);
  CHECK(media.at(AVMEDIA_TYPE_AUDIO).frames > 0);
}

TEST_CASE("LivePipeline rolls back rejected Processor and codec startup",
          "[pipeline][live][failure][startup]") {
  Runtime runtime;
  std::atomic<int> ready{0}, ended{0}, stopped{0};
  bool accepted = true;
  bool processor_prepared = true;
  auto config = Config();
  SECTION("Processor Start returns false") { accepted = false; }
  SECTION("Encoder cannot open the selected codec") {
    config.encoder.video_encoder_name = "missing-live-test-encoder";
  }
  SECTION("Scheduler Start returns false") {
    config.scheduler.video_frame_rate = {31, 2};
    config.encoder.fps = config.scheduler.video_frame_rate;
    processor_prepared = false;
  }
  LivePipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return accepted;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  CHECK_THROWS(pipeline.Start(Fixture()));
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(ready == (processor_prepared ? 1 : 0));
  CHECK(ended == 0);
  CHECK(stopped == (processor_prepared && accepted ? 1 : 0));
  CHECK_FALSE(pipeline.status().error_source.empty());
  CHECK_FALSE(pipeline.status().error_message.empty());
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
}

TEST_CASE("LivePipeline treats incompatible Processor audio as fatal",
          "[pipeline][live][failure][runtime]") {
  Runtime runtime;
  std::atomic<bool> corrupt{false};
  std::atomic<int> ended{0}, stopped{0};
  auto config = Config();
  config.input.loop = true;
  LivePipeline pipeline(config);
  pipeline.SetOnAudio([&](const auto& frame) noexcept {
    auto output = frame.Ref();
    if (corrupt) output->sample_rate = 32000;
    return output;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  pipeline.Start(Fixture());
  corrupt = true;
  REQUIRE(
      WaitUntil([&] { return pipeline.state() == PipelineState::kFailed; }));
  pipeline.Stop();
  CHECK(ended == 0);
  CHECK(stopped == 1);
  CHECK_FALSE(pipeline.status().error_source.empty());
  CHECK_FALSE(pipeline.status().error_message.empty());
}

TEST_CASE("LivePipeline unavailable push does not block local startup or Stop",
          "[pipeline][live][output][retry]") {
  Runtime runtime;
  OutputDirectory directory;
  std::atomic<int> videos{0}, ended{0}, stopped{0};
  auto config = Config();
  config.input.loop = true;
  LivePipeline pipeline(config);
  pipeline.AddPushUrl("rtsp://127.0.0.1:1/live/unavailable");
  const auto output = pipeline.AddPushUrl(directory.File());
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    ++videos;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  const auto before = std::chrono::steady_clock::now();
  REQUIRE_NOTHROW(pipeline.Start(Fixture()));
  CHECK(std::chrono::steady_clock::now() - before < 5s);
  CHECK(pipeline.state() == PipelineState::kRunning);
  REQUIRE(WaitUntil([&] { return videos >= 20; }));
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK(pipeline.status().error_message.empty());
  CHECK(ended == 0);
  CHECK(stopped == 1);
  const auto media = DecodeRecording(output);
  CHECK(media.at(AVMEDIA_TYPE_VIDEO).frames > 0);
  CHECK(media.at(AVMEDIA_TYPE_AUDIO).frames > 0);
}

TEST_CASE("LivePipeline Stop interrupts input startup retry",
          "[pipeline][live][startup][retry]") {
  Runtime runtime;
  std::atomic<int> ready{0}, ended{0}, stopped{0};
  auto config = Config();
  config.input.auto_reconnect = true;
  config.input.open_timeout = 250ms;
  config.input.retry_interval = 30s;
  config.input.max_retries = -1;
  LivePipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return true;
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  auto start = std::async(std::launch::async, [&] {
    pipeline.Start("rtsp://127.0.0.1:1/live/unavailable");
  });
  StopPipeline stop_on_failure{pipeline};
  REQUIRE(WaitUntil(
      [&] { return pipeline.state() == PipelineState::kReconnecting; }));
  CHECK(start.wait_for(100ms) == std::future_status::timeout);
  auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
  REQUIRE(stop.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(stop.get());
  REQUIRE(start.wait_for(5s) == std::future_status::ready);
  try {
    start.get();
    FAIL("启动重试被 Stop 中止后必须报告中止");
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_EXIT);
  }
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK(ready == 0);
  CHECK(ended == 0);
  CHECK(stopped == 0);
}

TEST_CASE("LivePipeline loop restart retains filters and clears output targets",
          "[pipeline][live][loop][restart][output]") {
  Runtime runtime;
  OutputDirectory directory;
  std::atomic<int> ready{0}, videos{0}, ended{0}, stopped{0}, backward{0};
  std::int64_t previous = AV_NOPTS_VALUE;
  auto config = Config();
  config.input.loop = true;
  LivePipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    previous = AV_NOPTS_VALUE;
    return true;
  });
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    if (previous != AV_NOPTS_VALUE && frame->pts <= previous) ++backward;
    previous = frame->pts;
    ++videos;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  const auto output = pipeline.AddPushUrl(directory.File());
  pipeline.Start(Fixture());
  // Cross at least one full source loop without recreating downstream stages.
  REQUIRE(WaitUntil([&] { return videos >= 90; }));
  CHECK(ready == 1);
  CHECK(ended == 0);
  // A second live chain starts before this RTSP target exists, then recovers
  // through Input's existing retry policy once publication is added.
  auto pulling_config = Config();
  pulling_config.input.auto_reconnect = true;
  pulling_config.input.open_timeout = 2s;
  pulling_config.input.retry_interval = 100ms;
  pulling_config.input.options["rtsp_transport"] = "tcp";
  LivePipeline pulling(pulling_config);
  std::atomic<int> pulled_video{0}, pulled_audio{0};
  pulling.SetOnVideo([&](const auto& frame) noexcept {
    ++pulled_video;
    return frame.Ref();
  });
  pulling.SetOnAudio([&](const auto& frame) noexcept {
    ++pulled_audio;
    return frame.Ref();
  });
  auto pulling_start = std::async(std::launch::async, [&] {
    pulling.Start("rtsp://127.0.0.1:28593/live/runtime");
  });
  StopPipeline stop_pulling{pulling};
  REQUIRE(WaitUntil(
      [&] { return pulling.state() == PipelineState::kReconnecting; }));
  CHECK(pulling_start.wait_for(50ms) == std::future_status::timeout);
  pipeline.AddRtspPublish("live", "runtime", "127.0.0.1", 28593);
  CHECK(pipeline.ready_info().rtsp_url ==
        "rtsp://127.0.0.1:28593/live/runtime");
  REQUIRE(pulling_start.wait_for(10s) == std::future_status::ready);
  REQUIRE_NOTHROW(pulling_start.get());
  CHECK(pulling.state() == PipelineState::kRunning);
  REQUIRE(WaitUntil([&] { return pulled_video >= 5 && pulled_audio >= 5; }));
  pulling.Stop();
  CHECK(pulling.state() == PipelineState::kStopped);
  pipeline.Stop();
  CHECK(stopped == 1);
  CHECK(backward == 0);
  const auto media = DecodeRecording(output);
  CHECK(media.at(AVMEDIA_TYPE_VIDEO).frames >= 85);
  CHECK(std::abs(media.at(AVMEDIA_TYPE_VIDEO).end -
                 media.at(AVMEDIA_TYPE_AUDIO).end) < 0.15);
  pipeline.Start(Fixture());
  const auto info = pipeline.ready_info();
  CHECK(info.output_urls.empty());
  CHECK(info.rtsp_url.empty());
  CHECK(info.hls_url.empty());
  pipeline.Stop();
  CHECK(ready == 2);
  CHECK(stopped == 2);
  CHECK(ended == 0);
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("LivePipeline defaults to the first video and audio in source order",
          "[pipeline][live][tracks][configuration]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false, true);
  auto config = Config();
  config.input.loop = true;
  std::atomic<int> selected{0};
  LivePipeline pipeline(config);
  const bool reset_callback = GENERATE(false, true);
  if (reset_callback) {
    pipeline.SetOnSelectStreams([&](const auto&) {
      ++selected;
      return std::vector<int>{3, 2};
    });
    pipeline.SetOnSelectStreams({});
  }
  pipeline.Start(source.path());
  CHECK(pipeline.state() == PipelineState::kRunning);
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  CHECK(info.streams[0].stream_index == 0);
  CHECK(info.streams[1].stream_index == 1);
  CHECK((info.streams[0].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK((info.streams[1].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK(selected == 0);
  CHECK_THROWS_AS(pipeline.SetOnSelectStreams({}), std::logic_error);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("LivePipeline rejects selection before downstream preparation",
          "[pipeline][live][tracks][configuration][failure]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false);
  std::atomic<int> selected{0}, ready{0}, frames{0};
  std::vector<int> offered;
  LivePipeline pipeline(Config());
  const bool throws = GENERATE(false, true);
  pipeline.SetOnSelectStreams([&](const auto& streams) -> std::vector<int> {
    ++selected;
    for (const auto& stream : streams) offered.push_back(stream.stream_index);
    if (throws) throw std::invalid_argument("selector rejected tracks");
    return {0, 123};
  });
  pipeline.SetOnReady([&](const auto&, const auto&) noexcept {
    ++ready;
    return true;
  });
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    ++frames;
    return frame.Ref();
  });
  pipeline.SetOnAudio([&](const auto& frame) noexcept {
    ++frames;
    return frame.Ref();
  });
  if (throws) {
    try {
      pipeline.Start(source.path());
      FAIL("selector exception must propagate through synchronous Start");
    } catch (const std::invalid_argument& error) {
      CHECK(std::string(error.what()) == "selector rejected tracks");
    }
  } else {
    CHECK_THROWS_AS(pipeline.Start(source.path()), ffmpeg::FfmpegException);
  }
  CHECK(selected == 1);
  CHECK(offered == std::vector<int>{0, 1, 2});
  CHECK(ready == 0);
  CHECK(frames == 0);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.status().error_source == "input");
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
}

TEST_CASE(
    "LivePipeline uses its selector before consuming the chosen audio track",
    "[pipeline][live][tracks][eof][control]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture());
  auto config = Config();
  const bool stop_actively = GENERATE(false, true);
  config.input.loop = stop_actively;
  std::atomic<int> selections{0};
  std::atomic<bool> all_tracks_offered{true}, selection_finished{false};
  std::atomic<bool> selection_before_consumption{true};
  LivePipeline pipeline(config);
  pipeline.SetOnSelectStreams([&](const auto& streams) {
    if (streams.size() != 3 || streams[0].stream_index != 0 ||
        streams[1].stream_index != 1 || streams[2].stream_index != 2) {
      all_tracks_offered = false;
    }
    ++selections;
    selection_finished = true;
    return std::vector<int>{0, 2};
  });
  std::atomic<int> videos{0}, audios{0}, ended{0}, stopped{0};
  pipeline.SetOnVideo([&](const auto& frame) noexcept {
    if (!selection_finished) selection_before_consumption = false;
    ++videos;
    return frame.Ref();
  });
  pipeline.SetOnAudio([&](const auto& frame) noexcept {
    if (!selection_finished) selection_before_consumption = false;
    ++audios;
    return frame.Ref();
  });
  pipeline.SetOnEnded([&]() noexcept { ++ended; });
  pipeline.SetOnStop([&]() noexcept { ++stopped; });
  StopPipeline stop_on_failure{pipeline};
  OutputDirectory directory;
  pipeline.AddPushUrl(directory.File());
  pipeline.Start(source.path());
  if (stop_actively)
    REQUIRE(WaitUntil([&] { return videos > 0 && audios > 0; }));
  if (!stop_actively)
    REQUIRE(
        WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(pipeline.state() ==
        (stop_actively ? PipelineState::kStopped : PipelineState::kEnded));
  CHECK(videos > 0);
  CHECK(audios > 0);
  CHECK(ended == (stop_actively ? 0 : 1));
  CHECK(stopped == 1);
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  CHECK(info.streams[0].stream_index == 0);
  CHECK(info.streams[1].stream_index == 2);
  CHECK((info.streams[1].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK(selections >= 1);
  CHECK(all_tracks_offered);
  CHECK(selection_before_consumption);
}

}  // namespace

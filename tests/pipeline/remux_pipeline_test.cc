#include "mw/streamer/pipeline/remux_pipeline.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "multitrack_fixture.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::PipelineState;
using mw::streamer::RemuxPipeline;
using mw::streamer::RemuxPipelineConfig;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kMicroseconds{1, 1000000};

class OutputDirectory final {
 public:
  OutputDirectory()
      : path_(
            std::filesystem::current_path() /
            ("remux-pipeline-" + std::to_string(std::chrono::steady_clock::now()
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

class Runtime final {
 public:
  explicit Runtime(const std::string& log_file = {}) {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    if (!log_file.empty()) {
      config.log.modules = {{"streamer", mw::log::LogLevel::kWarning}};
      config.log.async.enabled = false;
      config.log.rotating_file.enabled = true;
      config.log.rotating_file.path = log_file;
    }
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

std::string Fixture(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) + "/" + name;
}

bool WaitUntil(const std::function<bool()>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

struct Notifications final {
  std::mutex mutex;
  std::condition_variable changed;
  int ready = 0;
  std::vector<PipelineState> states;

  void Bind(RemuxPipeline& pipeline) {
    pipeline.SetOnReady([this](const auto&) {
      std::lock_guard lock(mutex);
      ++ready;
      changed.notify_all();
    });
    pipeline.SetOnStateChanged([this](const auto& status) {
      std::lock_guard lock(mutex);
      states.push_back(status.state);
      changed.notify_all();
    });
  }
  bool Wait(PipelineState state) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 10s, [&] {
      return std::find(states.begin(), states.end(), state) != states.end();
    });
  }
  int Count(PipelineState state) {
    std::lock_guard lock(mutex);
    return static_cast<int>(std::count(states.begin(), states.end(), state));
  }
  int ReadyCount() {
    std::lock_guard lock(mutex);
    return ready;
  }
};

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
  RemuxPipeline& pipeline;
  ~StopPipeline() { pipeline.Stop(); }
};

struct FormatCloser final {
  void operator()(AVFormatContext* format) const {
    avformat_close_input(&format);
  }
};

struct Media final {
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
};

Media ReadMedia(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开管线测试媒体");
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "探测管线测试媒体");
  Media result;
  for (unsigned int index = 0; index < raw->nb_streams; ++index) {
    const auto* stream = raw->streams[index];
    const auto type = stream->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_VIDEO) continue;
    result.streams.push_back({static_cast<int>(index),
                              ffmpeg::CodecParameters(*stream->codecpar),
                              stream->time_base});
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "读取管线测试媒体");
    if (std::any_of(result.streams.begin(), result.streams.end(),
                    [&](const auto& stream) {
                      return packet->stream_index == stream.stream_index;
                    })) {
      result.packets.push_back(std::move(packet));
    }
  }
  return result;
}

struct DecodedTrack final {
  std::vector<std::int64_t> pts;
  std::vector<std::uint64_t> hashes;
};

// Decode both recordings independently. Container AAC priming/discard hints
// are removed from decoder-owned copies to compare every coded audio frame.
std::map<AVMediaType, DecodedTrack> Decode(const Media& media) {
  std::map<AVMediaType, DecodedTrack> result;
  for (const auto& stream : media.streams) {
    const auto* parameters = stream.codec_parameters.get();
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) throw std::runtime_error("测试媒体解码器不可用");
    ffmpeg::CodecContext context(codec);
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context.get(), parameters),
        "设置参考解码器");
    context.get()->pkt_timebase = stream.time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context.get(), codec, nullptr), "打开参考解码器");
    auto& output = result[parameters->codec_type];
    ffmpeg::Frame frame;
    const auto receive = [&] {
      for (;;) {
        const int status = avcodec_receive_frame(context.get(), frame.get());
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
        ffmpeg::FfmpegException::throwIfError(status, "解码管线测试媒体");
        std::uint64_t hash = 14695981039346656037ULL;
        const auto append = [&](const std::uint8_t* bytes, std::size_t size) {
          for (std::size_t index = 0; index < size; ++index) {
            hash = (hash ^ bytes[index]) * 1099511628211ULL;
          }
        };
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
          const auto pixel_format = static_cast<AVPixelFormat>(frame->format);
          const int size = av_image_get_buffer_size(pixel_format, frame->width,
                                                    frame->height, 1);
          ffmpeg::FfmpegException::throwIfError(size, "计算图像大小");
          std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
          ffmpeg::FfmpegException::throwIfError(
              av_image_copy_to_buffer(bytes.data(), size, frame->data,
                                      frame->linesize, pixel_format,
                                      frame->width, frame->height, 1),
              "复制参考图像");
          append(bytes.data(), bytes.size());
        } else {
          const auto sample_format = static_cast<AVSampleFormat>(frame->format);
          const bool planar = av_sample_fmt_is_planar(sample_format) != 0;
          const int channels = frame->ch_layout.nb_channels;
          const auto size =
              static_cast<std::size_t>(frame->nb_samples) *
              static_cast<std::size_t>(av_get_bytes_per_sample(sample_format)) *
              static_cast<std::size_t>(planar ? 1 : channels);
          for (int plane = 0; plane < (planar ? channels : 1); ++plane) {
            append(frame->extended_data[plane], size);
          }
        }
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
          throw std::runtime_error("参考帧缺少时间戳");
        output.pts.push_back(av_rescale_q(frame->best_effort_timestamp,
                                          stream.time_base, kMicroseconds));
        output.hashes.push_back(hash);
        frame.Unref();
      }
    };
    for (const auto& packet : media.packets) {
      if (packet->stream_index != stream.stream_index) continue;
      auto coded = packet.Clone();
      av_packet_free_side_data(coded.get());
      coded->flags &= ~AV_PKT_FLAG_DISCARD;
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(context.get(), coded.get()), "提交参考包");
      receive();
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(context.get(), nullptr), "排空参考解码器");
    receive();
  }
  return result;
}

}  // namespace

TEST_CASE("RemuxPipeline默认原包模式且非法配置直接抛出",
          "[pipeline][remux][validation]") {
  Runtime runtime;
  RemuxPipelineConfig config;
  CHECK(config.input.mode == mw::streamer::InputMode::kRemux);
  RemuxPipeline pipeline(config);
  CHECK(pipeline.state() == PipelineState::kIdle);
  CHECK(pipeline.status().error_message.empty());
  CHECK_THROWS_AS(pipeline.Start(""), std::invalid_argument);
  CHECK(pipeline.state() == PipelineState::kIdle);
  CHECK_THROWS_AS(pipeline.AddPushUrl("http://unsupported/live/stream"),
                  std::invalid_argument);
  CHECK_THROWS_AS(pipeline.AddRtspPublish("valid", "valid", "127.0.0.1", 0),
                  std::invalid_argument);
  CHECK_THROWS_AS(
      pipeline.AddHlsPublish("../invalid", "valid", "127.0.0.1", 8080),
      std::invalid_argument);
  CHECK(pipeline.ready_info().output_urls.empty());
  config.input.mode = mw::streamer::InputMode::kLive;
  CHECK_THROWS_AS(RemuxPipeline(config), std::invalid_argument);
  config.input.mode = mw::streamer::InputMode::kRemux;
  config.remuxer.max_pending_bytes = 0;
  CHECK_THROWS_AS(RemuxPipeline(config), std::invalid_argument);
}

TEST_CASE("RemuxPipeline同步就绪后自动EOF收尾完整录制H264 H265和AAC",
          "[pipeline][remux][recording]") {
  Runtime runtime;
  OutputDirectory directory;
  std::string fixture;
  SECTION("H264和AAC") { fixture = "h264_aac.mp4"; }
  SECTION("H265含B帧和AAC") { fixture = "h265_aac.mp4"; }
  Notifications notifications;
  RemuxPipelineConfig config;
  RemuxPipeline pipeline(config);
  const auto planned_path = pipeline.AddPushUrl(directory.File());
  CHECK_FALSE(std::filesystem::exists(planned_path));
  notifications.Bind(pipeline);
  pipeline.Start(Fixture(fixture));
  CHECK(notifications.ReadyCount() == 1);
  const auto ready = pipeline.ready_info();
  REQUIRE(ready.streams.size() == 2);
  REQUIRE(ready.output_urls.size() == 1);
  CHECK(ready.output_urls.front() == planned_path);
  CHECK(ready.output_urls.front() != directory.File());
  REQUIRE(notifications.Wait(PipelineState::kEnded));
  CHECK(pipeline.state() == PipelineState::kEnded);
  CHECK(pipeline.status().error_message.empty());
  const auto reference = Decode(ReadMedia(Fixture(fixture)));
  const auto actual = Decode(ReadMedia(ready.output_urls.front()));
  REQUIRE(actual.size() == reference.size());
  for (const auto& [type, frames] : reference) {
    REQUIRE(actual.count(type) == 1);
    CHECK(actual.at(type).hashes == frames.hashes);
    REQUIRE(actual.at(type).pts.size() == frames.pts.size());
    for (std::size_t index = 0; index < frames.pts.size(); ++index) {
      const auto expected = frames.pts[index] - frames.pts.front();
      const auto recorded =
          actual.at(type).pts[index] - actual.at(type).pts.front();
      CHECK(std::abs(recorded - expected) <= 2000);
    }
  }
  CHECK(notifications.Count(PipelineState::kRunning) == 1);
  CHECK(notifications.Count(PipelineState::kStopping) == 1);
  CHECK(notifications.Count(PipelineState::kEnded) == 1);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kEnded);
  CHECK(notifications.Count(PipelineState::kEnded) == 1);
}

TEST_CASE("RemuxPipeline实际Input循环多代保持内容及单调DTS",
          "[pipeline][remux][generation]") {
  Runtime runtime;
  OutputDirectory directory;
  Notifications notifications;
  RemuxPipelineConfig config;
  config.input.loop = true;
  RemuxPipeline pipeline(config);
  pipeline.AddPushUrl(directory.File());
  notifications.Bind(pipeline);
  pipeline.Start(Fixture("h265_aac.mp4"));
  const auto path = pipeline.ready_info().output_urls.at(0);
  const auto minimum_size =
      std::filesystem::file_size(Fixture("h265_aac.mp4")) * 3;
  REQUIRE(WaitUntil([&] {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > minimum_size;
  }));
  CHECK(pipeline.state() == PipelineState::kRunning);
  CHECK(notifications.Count(PipelineState::kEnded) == 0);
  CHECK(notifications.ReadyCount() == 1);
  pipeline.Stop();
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK(notifications.Count(PipelineState::kStopped) == 1);
  const auto output = ReadMedia(path);
  std::map<int, std::int64_t> previous;
  for (const auto& packet : output.packets) {
    REQUIRE(packet->dts != AV_NOPTS_VALUE);
    if (previous.count(packet->stream_index))
      CHECK(packet->dts > previous.at(packet->stream_index));
    previous[packet->stream_index] = packet->dts;
  }
  const auto reference = Decode(ReadMedia(Fixture("h265_aac.mp4")));
  const auto actual = Decode(output);
  for (const auto& [type, frames] : reference) {
    REQUIRE(actual.count(type) == 1);
    const auto count = frames.hashes.size();
    REQUIRE(actual.at(type).hashes.size() >= count * 2);
    CHECK(actual.at(type).pts[count] > actual.at(type).pts[count - 1]);
    if (type == AVMEDIA_TYPE_VIDEO) {
      for (std::size_t index = 0; index < count * 2; ++index)
        CHECK(actual.at(type).hashes[index] == frames.hashes[index % count]);
    }
  }
}

TEST_CASE("RemuxPipeline主动停止后可重新同步启动",
          "[pipeline][remux][restart]") {
  Runtime runtime;
  Notifications notifications;
  RemuxPipelineConfig config;
  config.input.loop = true;
  RemuxPipeline pipeline(config);
  notifications.Bind(pipeline);
  for (int iteration = 0; iteration < 2; ++iteration) {
    pipeline.Start(Fixture());
    CHECK(pipeline.state() == PipelineState::kRunning);
    CHECK(notifications.ReadyCount() == iteration + 1);
    CHECK_THROWS_AS(pipeline.Start(Fixture()), std::logic_error);
    pipeline.Stop();
    CHECK(pipeline.state() == PipelineState::kStopped);
    CHECK(pipeline.status().error_message.empty());
  }
  CHECK(notifications.Count(PipelineState::kRunning) == 2);
  CHECK(notifications.Count(PipelineState::kStopped) == 2);
}

TEST_CASE("RemuxPipeline同步启动失败保留异常且清理后可恢复",
          "[pipeline][remux][failure]") {
  Runtime runtime;
  Notifications notifications;
  RemuxPipeline pipeline;
  notifications.Bind(pipeline);
  CHECK_THROWS_AS(pipeline.Start(Fixture("missing-pipeline-file.mp4")),
                  ffmpeg::FfmpegException);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.status().error_code != 0);
  CHECK_FALSE(pipeline.status().error_source.empty());
  CHECK_FALSE(pipeline.status().error_message.empty());
  CHECK(notifications.ReadyCount() == 0);
  pipeline.Start(Fixture());
  CHECK(notifications.ReadyCount() == 1);
  REQUIRE(notifications.Wait(PipelineState::kEnded));
  CHECK(pipeline.status().error_code == 0);
  CHECK(pipeline.status().error_source.empty());
  CHECK(pipeline.status().error_message.empty());
}

TEST_CASE("RemuxPipeline运行中添加录制并在Stop后清空目标",
          "[pipeline][remux][output][restart]") {
  Runtime runtime;
  OutputDirectory directory;
  RemuxPipelineConfig config;
  config.input.loop = true;
  RemuxPipeline pipeline(config);
  Notifications notifications;
  notifications.Bind(pipeline);
  pipeline.Start(Fixture());
  REQUIRE(pipeline.ready_info().output_urls.empty());
  const auto first = pipeline.AddPushUrl(directory.File());
  CHECK(pipeline.ready_info().output_urls == std::vector<std::string>{first});
  CHECK_THROWS_AS(pipeline.AddPushUrl("https://unsupported/push"),
                  std::invalid_argument);
  CHECK(pipeline.state() == PipelineState::kRunning);
  REQUIRE(WaitUntil([&] {
    std::error_code error;
    return std::filesystem::file_size(first, error) > 16 * 1024 && !error;
  }));
  pipeline.Stop();
  const auto first_size = std::filesystem::file_size(first);
  const auto decoded = Decode(ReadMedia(first));
  REQUIRE(decoded.count(AVMEDIA_TYPE_VIDEO) == 1);
  REQUIRE(decoded.count(AVMEDIA_TYPE_AUDIO) == 1);
  CHECK_FALSE(decoded.at(AVMEDIA_TYPE_VIDEO).hashes.empty());
  CHECK_FALSE(decoded.at(AVMEDIA_TYPE_AUDIO).hashes.empty());

  pipeline.Start(Fixture());
  CHECK(pipeline.ready_info().output_urls.empty());
  pipeline.Stop();
  const auto second = pipeline.AddPushUrl(directory.File());
  REQUIRE(second != first);
  CHECK_FALSE(std::filesystem::exists(second));
  pipeline.Start(Fixture());
  CHECK(pipeline.ready_info().output_urls == std::vector<std::string>{second});
  pipeline.Stop();
  CHECK(std::filesystem::file_size(first) == first_size);
  CHECK(notifications.ReadyCount() == 3);
}

TEST_CASE("RemuxPipeline未启动时Stop也清空待启用输出",
          "[pipeline][remux][output][lifecycle]") {
  Runtime runtime;
  OutputDirectory directory;
  RemuxPipeline pipeline;
  const auto planned = pipeline.AddPushUrl(directory.File());
  CHECK_FALSE(std::filesystem::exists(planned));
  pipeline.Stop();
  pipeline.Start(Fixture());
  CHECK(pipeline.ready_info().output_urls.empty());
  pipeline.Stop();
  CHECK_FALSE(std::filesystem::exists(planned));
}

TEST_CASE("RemuxPipeline循环重开失败自动完整停止后可重新启动",
          "[pipeline][remux][failure]") {
  Runtime runtime;
  OutputDirectory directory;
  const auto source = directory.File("source.mp4");
  std::filesystem::copy_file(Fixture(), source);
  Notifications notifications;
  RemuxPipelineConfig config;
  config.input.loop = true;
  config.input.auto_reconnect = false;
  SECTION("关闭自动重连时直接失败") {}
  SECTION("重连次数耗尽后失败") {
    config.input.auto_reconnect = true;
    config.input.max_retries = 1;
    config.input.retry_interval = 20ms;
  }
  RemuxPipeline pipeline(config);
  pipeline.AddPushUrl(directory.File());
  notifications.Bind(pipeline);
  pipeline.Start(source);
  const auto output = pipeline.ready_info().output_urls.at(0);
  {
    std::ofstream truncated(source, std::ios::binary | std::ios::trunc);
    REQUIRE(truncated.good());
  }
  REQUIRE(notifications.Wait(PipelineState::kFailed));
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK_FALSE(pipeline.status().error_source.empty());
  CHECK_FALSE(pipeline.status().error_message.empty());
  CHECK(notifications.Count(PipelineState::kFailed) == 1);
  CHECK(notifications.Count(PipelineState::kReconnecting) ==
        (config.input.auto_reconnect ? 1 : 0));
  CHECK_FALSE(ReadMedia(output).packets.empty());
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
  std::filesystem::copy_file(Fixture(), source,
                             std::filesystem::copy_options::overwrite_existing);
  pipeline.Start(source);
  CHECK(pipeline.state() == PipelineState::kRunning);
  CHECK(pipeline.status().error_message.empty());
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kStopped);
}

TEST_CASE("RemuxPipeline同步输出准备失败抛出原始异常并完整清理",
          "[pipeline][remux][failure][output]") {
  Runtime runtime;
  Notifications notifications;
  OutputDirectory directory;
  const auto blocked = directory.File("blocked");
  {
    std::ofstream file(blocked);
    REQUIRE(file.good());
  }
  RemuxPipeline pipeline;
  pipeline.AddPushUrl(blocked + "/record.mp4");
  notifications.Bind(pipeline);
  CHECK_THROWS_AS(pipeline.Start(Fixture()), std::runtime_error);
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.status().error_source == "remuxer");
  CHECK_FALSE(pipeline.status().error_message.empty());
  CHECK(notifications.ReadyCount() == 0);
  CHECK(notifications.Count(PipelineState::kFailed) == 1);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(notifications.Count(PipelineState::kFailed) == 1);
  Notifications recovered;
  RemuxPipeline valid_pipeline;
  recovered.Bind(valid_pipeline);
  REQUIRE_NOTHROW(valid_pipeline.Start(Fixture()));
  REQUIRE(recovered.Wait(PipelineState::kEnded));
  CHECK(valid_pipeline.status().error_message.empty());
}

TEST_CASE("RemuxPipeline启动缓存不足的空目标错误自动停止整条管线",
          "[pipeline][remux][failure][startup]") {
  Runtime runtime;
  const auto source = ReadMedia(Fixture());
  const auto packet_bytes = [](const ffmpeg::Packet& packet) {
    std::size_t bytes = static_cast<std::size_t>(packet->size);
    for (int index = 0; index < packet->side_data_elems; ++index)
      bytes += packet->side_data[index].size;
    return bytes;
  };
  RemuxPipelineConfig config;
  config.remuxer.max_pending_bytes = 0;
  for (const auto& packet : source.packets) {
    config.remuxer.max_pending_bytes =
        std::max(config.remuxer.max_pending_bytes, packet_bytes(packet));
  }
  REQUIRE(source.packets.size() >= 2);
  REQUIRE(source.packets[0]->stream_index != source.packets[1]->stream_index);
  // Every single packet fits. The first audio/video pair cannot coexist in
  // startup storage, forcing OnError rather than an oversized-packet throw.
  REQUIRE(packet_bytes(source.packets[0]) + packet_bytes(source.packets[1]) >
          config.remuxer.max_pending_bytes);
  Notifications notifications;
  RemuxPipeline pipeline(config);
  notifications.Bind(pipeline);
  try {
    pipeline.Start(Fixture());
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_EXTERNAL);
  }
  REQUIRE(notifications.Wait(PipelineState::kFailed));
  CHECK(pipeline.state() == PipelineState::kFailed);
  const auto failed = pipeline.status();
  CHECK(failed.error_source == "remuxer");
  CHECK(failed.error_code == AVERROR_EXTERNAL);
  CHECK(failed.error_message.find("缓存不足以等待各轨时间线就绪") !=
        std::string::npos);
  CHECK(notifications.Count(PipelineState::kFailed) == 1);
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
}

TEST_CASE("RemuxPipeline业务回调异常不会终止媒体任务",
          "[pipeline][remux][callbacks]") {
  Runtime runtime;
  std::atomic<int> ready{0};
  std::atomic<int> ended{0};
  RemuxPipeline pipeline;
  pipeline.SetOnReady([&](const auto&) {
    ++ready;
    throw std::runtime_error("测试就绪回调异常");
  });
  pipeline.SetOnStateChanged([&](const auto& status) {
    if (status.state == PipelineState::kEnded) ++ended;
    throw std::runtime_error("测试状态回调异常");
  });
  REQUIRE_NOTHROW(pipeline.Start(Fixture()));
  REQUIRE(WaitUntil([&] { return ended == 1; }));
  CHECK(ready == 1);
  CHECK(pipeline.state() == PipelineState::kEnded);
}

TEST_CASE("RemuxPipeline同步Start等待准备完成且启动中可请求Stop",
          "[pipeline][remux][startup][control]") {
  Runtime runtime;
  CallbackGate gate;
  RemuxPipelineConfig config;
  config.input.loop = true;
  RemuxPipeline pipeline(config);
  pipeline.SetOnReady([&](const auto&) { gate.Enter(); });
  auto start =
      std::async(std::launch::async, [&] { pipeline.Start(Fixture()); });
  // Release on assertion failures before the asynchronous call is joined.
  ReleaseGate release_start{gate};
  REQUIRE(gate.Wait());
  CHECK(pipeline.state() == PipelineState::kStarting);
  CHECK(start.wait_for(100ms) == std::future_status::timeout);
  SECTION("Start等待就绪回调完成和首包被接收") {
    gate.Release();
    REQUIRE(start.wait_for(5s) == std::future_status::ready);
    REQUIRE_NOTHROW(start.get());
    CHECK(pipeline.state() == PipelineState::kRunning);
    pipeline.Stop();
    CHECK(pipeline.state() == PipelineState::kStopped);
  }
  SECTION("Stop等待正在执行的准备回调并结束同步Start") {
    auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
    ReleaseGate release_stop{gate};
    REQUIRE(WaitUntil(
        [&] { return pipeline.state() == PipelineState::kStopping; }));
    CHECK(stop.wait_for(100ms) == std::future_status::timeout);
    CHECK(start.wait_for(0ms) == std::future_status::timeout);
    gate.Release();
    REQUIRE(stop.wait_for(5s) == std::future_status::ready);
    REQUIRE_NOTHROW(stop.get());
    REQUIRE(start.wait_for(5s) == std::future_status::ready);
    try {
      start.get();
      FAIL("启动被主动停止后Start必须报告中止");
    } catch (const ffmpeg::FfmpegException& error) {
      CHECK(error.error_code() == AVERROR_EXIT);
    }
    CHECK(pipeline.state() == PipelineState::kStopped);
    CHECK(pipeline.status().error_message.empty());
  }
}

TEST_CASE("RemuxPipeline单个网络输出失败不终止整个会话",
          "[pipeline][remux][output]") {
  OutputDirectory directory;
  const auto log_file = directory.File("pipeline.log");
  const std::string unavailable = "rtmp://127.0.0.1:1/live/unavailable";
  {
    Runtime runtime(log_file);
    RemuxPipelineConfig config;
    config.input.loop = true;
    config.remuxer.max_pending_bytes = 64 * 1024;
    RemuxPipeline pipeline(config);
    pipeline.AddPushUrl(unavailable);
    pipeline.Start(Fixture());
    // Wait for the actual refused-connection callback. The synchronous sink
    // buffers file writes, so probe records make preceding errors visible
    // without stopping the still-running pipeline to flush the logger.
    const std::string probe(4096, '.');
    REQUIRE(WaitUntil([&] {
      mw::log::Write("streamer", mw::log::LogLevel::kWarning, __FILE__,
                     __LINE__, probe);
      std::ifstream file(log_file);
      const std::string text((std::istreambuf_iterator<char>(file)), {});
      return text.find("RemuxPipeline 输出错误: target=" + unavailable) !=
             std::string::npos;
    }));
    CHECK(pipeline.state() == PipelineState::kRunning);
    CHECK(pipeline.status().error_message.empty());
    pipeline.Stop();
    CHECK(pipeline.state() == PipelineState::kStopped);
  }
  std::ifstream file(log_file);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  CHECK(text.find(unavailable) != std::string::npos);
}

TEST_CASE("RemuxPipeline各网络协议初次失败重连且Stop结束同步Start等待",
          "[pipeline][remux][startup][control]") {
  const std::string protocol =
      GENERATE("http", "https", "rtsp", "rtsps", "rtmp", "rtmps", "srt");
  INFO("输入协议=" << protocol);
  const auto url = protocol + "://127.0.0.1:1/live/unavailable";
  Runtime runtime;
  Notifications notifications;
  RemuxPipelineConfig config;
  config.input.open_timeout = 250ms;
  config.input.retry_interval = 30s;
  config.input.max_retries = -1;
  RemuxPipeline pipeline(config);
  notifications.Bind(pipeline);
  auto start = std::async(std::launch::async, [&] { pipeline.Start(url); });
  StopPipeline stop_on_failure{pipeline};
  REQUIRE(notifications.Wait(PipelineState::kReconnecting));
  CHECK(start.wait_for(0ms) == std::future_status::timeout);
  auto stop = std::async(std::launch::async, [&] { pipeline.Stop(); });
  REQUIRE(stop.wait_for(5s) == std::future_status::ready);
  REQUIRE_NOTHROW(stop.get());
  REQUIRE(start.wait_for(5s) == std::future_status::ready);
  try {
    start.get();
    FAIL("初始网络重试被主动停止后Start必须报告中止");
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == AVERROR_EXIT);
  }
  CHECK(pipeline.state() == PipelineState::kStopped);
  CHECK(notifications.ReadyCount() == 0);
  CHECK(notifications.Count(PipelineState::kFailed) == 0);
}

TEST_CASE("RemuxPipeline defaults to the first video and audio in source order",
          "[pipeline][remux][tracks][configuration]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false, true);
  auto config = RemuxPipelineConfig{};
  config.input.loop = true;
  std::atomic<int> selected{0};
  RemuxPipeline pipeline(config);
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

TEST_CASE("RemuxPipeline rejects selection before downstream preparation",
          "[pipeline][remux][tracks][configuration][failure]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture(), false);
  std::atomic<int> selected{0}, ready{0};
  std::vector<int> offered;
  OutputDirectory directory;
  RemuxPipeline pipeline(RemuxPipelineConfig{});
  const auto planned_recording = pipeline.AddPushUrl(directory.File());
  const bool throws = GENERATE(false, true);
  pipeline.SetOnSelectStreams([&](const auto& streams) -> std::vector<int> {
    ++selected;
    for (const auto& stream : streams) offered.push_back(stream.stream_index);
    if (throws) throw std::invalid_argument("selector rejected tracks");
    return {0, 123};
  });
  pipeline.SetOnReady([&](const auto&) { ++ready; });
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
  CHECK_FALSE(std::filesystem::exists(planned_recording));
  CHECK(pipeline.state() == PipelineState::kFailed);
  CHECK(pipeline.status().error_source == "input");
  pipeline.Stop();
  CHECK(pipeline.state() == PipelineState::kFailed);
}

TEST_CASE(
    "RemuxPipeline uses its selector before consuming the chosen audio track",
    "[pipeline][remux][tracks][eof][control]") {
  Runtime runtime;
  mw::streamer::test::MultiTrackFixture source(Fixture());
  auto config = RemuxPipelineConfig{};
  const bool stop_actively = GENERATE(false, true);
  config.input.loop = stop_actively;
  std::atomic<int> selections{0};
  std::atomic<bool> all_tracks_offered{true}, selection_finished{false};
  std::atomic<bool> selection_before_consumption{true};
  RemuxPipeline pipeline(config);
  pipeline.SetOnSelectStreams([&](const auto& streams) {
    if (streams.size() != 3 || streams[0].stream_index != 0 ||
        streams[1].stream_index != 1 || streams[2].stream_index != 2) {
      all_tracks_offered = false;
    }
    ++selections;
    selection_finished = true;
    return std::vector<int>{0, 2};
  });
  pipeline.SetOnReady([&](const auto&) {
    if (!selection_finished) selection_before_consumption = false;
  });
  OutputDirectory directory;
  pipeline.AddPushUrl(directory.File());
  pipeline.Start(source.path());
  if (!stop_actively)
    REQUIRE(
        WaitUntil([&] { return pipeline.state() == PipelineState::kEnded; }));
  pipeline.Stop();
  CHECK(pipeline.state() ==
        (stop_actively ? PipelineState::kStopped : PipelineState::kEnded));
  const auto info = pipeline.ready_info();
  REQUIRE(info.streams.size() == 2);
  CHECK(info.streams[0].stream_index == 0);
  CHECK(info.streams[1].stream_index == 2);
  CHECK((info.streams[1].disposition & AV_DISPOSITION_COMMENT) != 0);
  CHECK(selections >= 1);
  CHECK(all_tracks_offered);
  CHECK(selection_before_consumption);
}

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/mathematics.h>
}

#include "mw/log.h"
#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/init/init.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::AvEncoder;
using mw::streamer::AvEncoderConfig;
using namespace std::chrono_literals;

class EncoderLatencyLog final {
 public:
  EncoderLatencyLog()
      : directory_(
            fs::temp_directory_path() /
            ("mw-encoder-latency-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        path_(directory_ / "latency.log"),
        runtime_(nullptr, &mw::streamer::Shutdown) {
    fs::create_directories(directory_);
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = path_.string();
    config.log.modules = {{"streamer", mw::log::LogLevel::kOff}};
    config.performance.stages = {
        {mw::streamer::PerformanceStage::kEncoderVideo,
         mw::streamer::PerformanceMode::kDetail},
        {mw::streamer::PerformanceStage::kEncoderAudio,
         mw::streamer::PerformanceMode::kDetail},
        {mw::streamer::PerformanceStage::kEncoderAudioFilter,
         mw::streamer::PerformanceMode::kDetail}};
    runtime_.reset(mw::streamer::Init(config));
  }
  ~EncoderLatencyLog() {
    runtime_.reset();
    if (directory_.parent_path() == fs::temp_directory_path()) {
      std::error_code error;
      fs::remove_all(directory_, error);
    }
  }
  std::string Read() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }

 private:
  fs::path directory_;
  fs::path path_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

AvEncoderConfig VideoConfig() {
  AvEncoderConfig config;
  config.width = 64;
  config.height = 64;
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.video_options = {{"preset", "ultrafast"}, {"tune", "zerolatency"}};
  return config;
}

ffmpeg::Frame VideoFrame(std::uint8_t marker) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = frame->height = 64;
  frame->pts = 123000 + static_cast<std::int64_t>(marker) * 3000;
  frame->time_base = {1, 90000};
  frame->duration = 3000;
  if (av_frame_get_buffer(frame.get(), 0) < 0)
    throw std::runtime_error("分配 latency 视频测试帧失败");
  for (int plane = 0; plane < 2; ++plane) {
    for (int row = 0; row < (plane == 0 ? 64 : 32); ++row)
      std::memset(frame->data[plane] + row * frame->linesize[plane],
                  plane == 0 ? 80 : 128, 64);
  }
  frame->opaque_ref = av_buffer_alloc(1);
  if (!frame->opaque_ref) throw std::bad_alloc();
  frame->opaque_ref->data[0] = marker;
  return frame;
}

ffmpeg::Frame AudioFrame() {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = 2048;
  frame->pts = 480000;
  frame->time_base = {1, 48000};
  frame->duration = frame->nb_samples;
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0)
    throw std::runtime_error("分配 latency 音频测试帧失败");
  for (int channel = 0; channel < 2; ++channel)
    std::memset(frame->extended_data[channel], 0, 2048 * sizeof(float));
  frame->opaque_ref = av_buffer_alloc(1);
  if (!frame->opaque_ref) throw std::bad_alloc();
  frame->opaque_ref->data[0] = 91;
  return frame;
}

bool CopiesOpaque(const AVCodec& codec) {
#if defined(AV_CODEC_FLAG_COPY_OPAQUE) && \
    defined(AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE)
  return codec.capabilities & AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE;
#else
  (void)codec;
  return false;
#endif
}

std::string Summary(const std::string& log, const std::string& stage) {
  std::string result;
  std::size_t begin = 0;
  while (begin < log.size()) {
    const auto end = log.find('\n', begin);
    const auto line = log.substr(begin, end - begin);
    if (line.find("stage=" + stage + " ") != std::string::npos &&
        line.find("report=summary") != std::string::npos)
      result = line;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  REQUIRE_FALSE(result.empty());
  return result;
}

std::uint64_t Count(const std::string& line, const char* field) {
  std::smatch match;
  REQUIRE(std::regex_search(line, match,
                            std::regex(std::string(field) + "=([0-9]+)")));
  return std::stoull(match[1].str());
}

void CheckThroughput(const std::string& line, std::uint64_t count) {
  INFO(line);
  std::smatch match;
  REQUIRE(
      std::regex_search(line, match, std::regex(R"(throughput=([0-9.]+)/s)")));
  const double rate = std::stod(match[1].str());
  REQUIRE(std::isfinite(rate));
  REQUIRE(rate > 0);
  REQUIRE(std::regex_search(line, match, std::regex(R"(window_s=([0-9.]+))")));
  const double seconds = std::stod(match[1].str());
  // Both fields round to three decimal places. Bound the rate using the
  // displayed window's rounding interval, including very short sessions.
  constexpr double kRounding = 0.000501;
  CHECK(rate + kRounding >= static_cast<double>(count) / (seconds + kRounding));
  if (seconds > kRounding) {
    CHECK(rate - kRounding <=
          static_cast<double>(count) / (seconds - kRounding));
  }
}

}  // namespace

TEST_CASE("视频编码 latency 跟随 B 帧重排并保留原始 opaque",
          "[performance][latency][encoder]") {
  EncoderLatencyLog log;
  const auto* codec = avcodec_find_encoder_by_name("libx264");
  REQUIRE(codec != nullptr);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  AvEncoder encoder(cpu);
  std::vector<ffmpeg::Packet> packets;
  encoder.SetOnPacket(
      [&](const ffmpeg::Packet& packet) { packets.push_back(packet.Ref()); });
  auto config = VideoConfig();
  config.max_b_frames = 2;
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=0:sync-lookahead=0";
  REQUIRE(encoder.Start(config));
  std::vector<ffmpeg::Frame> sources;
  for (std::uint8_t index = 0; index < 12; ++index) {
    sources.push_back(VideoFrame(index));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    // Retry the same frame until admitted; a full queue must not change its
    // timestamp or opaque source association.
    while (!encoder.TryPushVideo(sources.back())) {
      REQUIRE_FALSE(encoder.error());
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  }
  encoder.Stop();
  REQUIRE_FALSE(encoder.error());
  REQUIRE(packets.size() == sources.size());
  bool reordered = false;
  for (std::size_t index = 0; index < packets.size(); ++index) {
    const auto& packet = packets[index];
    const auto source = std::find_if(
        sources.begin(), sources.end(), [&](const ffmpeg::Frame& frame) {
          return packet->pts ==
                 av_rescale_q(frame->pts, frame->time_base, packet->time_base);
        });
    REQUIRE(source != sources.end());
    if (index && packet->pts < packets[index - 1]->pts) reordered = true;
    if (CopiesOpaque(*codec)) {
      REQUIRE(packet->opaque_ref != nullptr);
      REQUIRE(packet->opaque_ref->data == (*source)->opaque_ref->data);
      REQUIRE(packet->opaque_ref->data[0] ==
              static_cast<std::uint8_t>(source - sources.begin()));
    }
  }
  REQUIRE(reordered);
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const auto& frame = sources[index];
    REQUIRE(frame->pts == 123000 + static_cast<std::int64_t>(index) * 3000);
    REQUIRE(frame->time_base.num == 1);
    REQUIRE(frame->time_base.den == 90000);
    REQUIRE(frame->duration == 3000);
    REQUIRE(frame->opaque_ref->size == 1);
    REQUIRE(frame->opaque_ref->data[0] == index);
  }
  const auto summary = Summary(log.Read(), "encode.video");
  INFO(summary);
  REQUIRE(Count(summary, "samples") == packets.size());
  REQUIRE(Count(summary, "unavailable") ==
          (CopiesOpaque(*codec) ? 0 : packets.size()));
}

TEST_CASE("当前编码包的 latency 在进入阻塞回调前结束",
          "[performance][latency][encoder]") {
  EncoderLatencyLog log;
  const auto* codec = avcodec_find_encoder_by_name("libx264");
  REQUIRE(codec != nullptr);
  if (!CopiesOpaque(*codec)) SKIP("libx264 不支持 opaque 传播");
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false;
  bool release = false;
  AvEncoder encoder(cpu);
  encoder.SetOnPacket([&](const ffmpeg::Packet&) {
    std::unique_lock lock(mutex);
    entered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return release; });
  });
  REQUIRE(encoder.Start(VideoConfig()));
  const auto started = std::chrono::steady_clock::now();
  REQUIRE(encoder.TryPushVideo(VideoFrame(1)));
  bool received = false;
  {
    std::unique_lock lock(mutex);
    received = wake.wait_for(lock, 10s, [&] { return entered; });
  }
  const auto before_callback = std::chrono::steady_clock::now() - started;
  // Hold longer than the whole pre-callback interval; no fixed codec speed
  // assumption is needed to prove the callback wait is outside this sample.
  if (received) std::this_thread::sleep_for(before_callback + 50ms);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  wake.notify_all();
  encoder.Stop();
  REQUIRE(received);
  REQUIRE_FALSE(encoder.error());
  const auto summary = Summary(log.Read(), "encode.video");
  INFO(summary);
  REQUIRE(Count(summary, "samples") == 1);
  REQUIRE(Count(summary, "unavailable") == 0);
  std::smatch match;
  REQUIRE(std::regex_search(summary, match, std::regex(R"(max:([0-9.]+)ms)")));
  const double maximum_ms = std::stod(match[1].str());
  const double before_ms =
      std::chrono::duration<double, std::milli>(before_callback).count();
  REQUIRE(maximum_ms <= before_ms + 1.0);
}

TEST_CASE("音频编码不支持关联时明确报告 N/A 且保留滤镜来源",
          "[performance][latency][encoder]") {
  EncoderLatencyLog log;
  const auto* codec = avcodec_find_encoder_by_name("aac");
  REQUIRE(codec != nullptr);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  AvEncoder encoder(cpu);
  std::size_t packets = 0;
  encoder.SetOnPacket([&](const ffmpeg::Packet&) { ++packets; });
  auto config = VideoConfig();
  config.video_pixel_format = AV_PIX_FMT_NONE;
  config.audio_sample_format = AV_SAMPLE_FMT_FLTP;
  REQUIRE(encoder.Start(config));
  auto frame = AudioFrame();
  auto* original = frame->opaque_ref->data;
  REQUIRE(encoder.TryPushAudio(frame));
  encoder.Stop();
  REQUIRE_FALSE(encoder.error());
  REQUIRE(packets > 0);
  REQUIRE(frame->pts == 480000);
  REQUIRE(frame->time_base.num == 1);
  REQUIRE(frame->time_base.den == 48000);
  REQUIRE(frame->duration == 2048);
  REQUIRE(frame->opaque_ref->data == original);
  REQUIRE(frame->opaque_ref->size == 1);
  REQUIRE(frame->opaque_ref->data[0] == 91);
  const auto contents = log.Read();
  const auto summary = Summary(contents, "encode.audio");
  INFO(summary);
  REQUIRE(Count(summary, "samples") == packets);
  CheckThroughput(summary, static_cast<std::uint64_t>(packets));
  if (!CopiesOpaque(*codec)) {
    REQUIRE(Count(summary, "unavailable") == packets);
    REQUIRE(summary.find("latency(mean:N/A,max:N/A)") != std::string::npos);
  }
  const auto filter = Summary(contents, "filter.encoder.audio");
  INFO(filter);
  REQUIRE(Count(filter, "samples") == 2);
  REQUIRE(Count(filter, "unavailable") == 0);
  CheckThroughput(filter, 2);
}

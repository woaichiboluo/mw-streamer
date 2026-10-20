#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mw/log.h"
#include "mw/streamer/filter/audio_resampler.h"
#include "mw/streamer/filter/force_fps.h"
#include "mw/streamer/init/init.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::AudioResampler;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::ForceFps;

class ConverterLatencyLog final {
 public:
  ConverterLatencyLog()
      : directory_(
            fs::temp_directory_path() /
            ("mw-converter-latency-" +
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
        {mw::streamer::PerformanceStage::kForceFps,
         mw::streamer::PerformanceMode::kDetail},
        {mw::streamer::PerformanceStage::kAudioResampler,
         mw::streamer::PerformanceMode::kDetail}};
    runtime_.reset(mw::streamer::Init(config));
  }
  ~ConverterLatencyLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove_all(directory_, error);
  }
  std::vector<std::uint64_t> Counts(const std::string& stage) {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    std::vector<std::uint64_t> counts;
    std::string line;
    while (std::getline(file, line)) {
      if (line.find("stage=" + stage + " ") == std::string::npos ||
          line.find("report=summary") == std::string::npos)
        continue;
      INFO(line);
      REQUIRE(line.find("unavailable=0 ") != std::string::npos);
      std::smatch match;
      REQUIRE(
          std::regex_search(line, match, std::regex(R"(samples=([0-9]+))")));
      counts.push_back(std::stoull(match[1].str()));
    }
    return counts;
  }

 private:
  fs::path directory_;
  fs::path path_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

void AttachOpaque(ffmpeg::Frame& frame) {
  frame->opaque_ref = av_buffer_alloc(1);
  if (!frame->opaque_ref) throw std::bad_alloc();
  frame->opaque_ref->data[0] = 73;
}

AvDecoderReadResult Video(std::int64_t pts) {
  ffmpeg::Frame frame;
  frame->width = frame->height = 8;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->pts = pts;
  if (av_frame_get_buffer(frame.get(), 0) < 0)
    throw std::runtime_error("分配转换 latency 视频帧失败");
  AttachOpaque(frame);
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kFrame;
  input.time_base = {1, 1000};
  input.frame = std::move(frame);
  return input;
}

AvDecoderReadResult Audio() {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 44100;
  frame->nb_samples = 1024;
  frame->pts = 0;
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0)
    throw std::runtime_error("分配转换 latency 音频帧失败");
  for (int channel = 0; channel < 2; ++channel)
    std::memset(frame->extended_data[channel], 0, 1024 * sizeof(float));
  AttachOpaque(frame);
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kFrame;
  input.time_base = {1, 44100};
  input.frame = std::move(frame);
  return input;
}

AvDecoderReadResult Eof(AVRational time_base, std::int64_t end_pts) {
  AvDecoderReadResult input;
  input.status = AvDecoderReadStatus::kEof;
  input.time_base = time_base;
  input.end_pts = end_pts;
  return input;
}

}  // namespace

TEST_CASE("ForceFps latency 按实际输出计数且 Reset 分会话",
          "[performance][latency][converter]") {
  ConverterLatencyLog log;
  {
    ForceFps fps;
    auto input = Video(0);
    const auto* original = (*input.frame)->opaque_ref->data;
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
    REQUIRE(fps.TryPushFrame(input));
    REQUIRE(fps.TryPushFrame(Video(120)));
    REQUIRE(fps.TryPushFrame(Eof({1, 1000}, 160)));
    for (int index = 0; index < 4; ++index) {
      auto output = fps.TryReadFrame();
      REQUIRE(output.status == AvDecoderReadStatus::kFrame);
      REQUIRE((*output.frame)->opaque_ref != nullptr);
      REQUIRE((*output.frame)->opaque_ref->data[0] == 73);
      if (index < 3) REQUIRE((*output.frame)->opaque_ref->data == original);
    }
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kEof);
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
    REQUIRE((*input.frame)->opaque_ref->data == original);
    // Reset closes an active session; downsampling contributes no extra samples
    // for discarded inputs or EOF markers in the next session.
    fps.Reset();
    for (const auto pts : {0, 10, 20, 40})
      REQUIRE(fps.TryPushFrame(Video(pts)));
    REQUIRE(fps.TryPushFrame(Eof({1, 1000}, 80)));
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kFrame);
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kFrame);
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kEof);
    fps.Stop();
    fps.Stop();
    REQUIRE(fps.TryReadFrame().status == AvDecoderReadStatus::kNull);
  }
  REQUIRE(log.Counts("filter.force_fps") == std::vector<std::uint64_t>{4, 2});
}

TEST_CASE("AudioResampler latency 包含 EOF 排空帧且停止后可重置",
          "[performance][latency][converter]") {
  ConverterLatencyLog log;
  std::vector<std::uint64_t> expected;
  {
    AudioResampler resampler;
    for (int session = 0; session < 2; ++session) {
      if (session) resampler.Reset();
      REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
      REQUIRE_THROWS_AS(resampler.TryPushFrame({}), std::invalid_argument);
      auto input = Audio();
      const auto* original = (*input.frame)->opaque_ref->data;
      REQUIRE(resampler.TryPushFrame(input));
      auto output = resampler.TryReadFrame();
      REQUIRE(output.status == AvDecoderReadStatus::kFrame);
      REQUIRE((*output.frame)->opaque_ref->data == original);
      std::uint64_t count = 1;
      REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
      REQUIRE(resampler.TryPushFrame(Eof({1, 44100}, 1024)));
      bool eof = false;
      for (int iteration = 0; iteration < 8; ++iteration) {
        output = resampler.TryReadFrame();
        if (output.status == AvDecoderReadStatus::kEof) {
          eof = true;
          break;
        }
        REQUIRE(output.status == AvDecoderReadStatus::kFrame);
        REQUIRE((*output.frame)->opaque_ref->data == original);
        ++count;
      }
      REQUIRE(eof);
      REQUIRE(count > 1);
      REQUIRE((*input.frame)->opaque_ref->data == original);
      REQUIRE((*input.frame)->pts == 0);
      REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
      expected.push_back(count);
      resampler.Stop();
      resampler.Stop();
      REQUIRE(resampler.TryReadFrame().status == AvDecoderReadStatus::kNull);
    }
  }
  REQUIRE(log.Counts("filter.audio_resampler") == expected);
}

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "mw/log.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/syncer/syncer.h"

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::Syncer;
using mw::streamer::SyncerConfig;

class SyncerLatencyLog final {
 public:
  SyncerLatencyLog()
      : directory_(
            fs::temp_directory_path() /
            ("mw-syncer-latency-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))),
        path_(directory_ / "latency.log"),
        runtime_(nullptr, &mw::streamer::Shutdown) {
    fs::create_directories(directory_);
    mw::streamer::InitConfig config;
    config.event_poller_threads = config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = path_.string();
    config.log.modules = {{"streamer", mw::log::LogLevel::kOff},
                          {"perf.syncer.video", mw::log::LogLevel::kInfo}};
    runtime_.reset(mw::streamer::Init(config));
  }
  ~SyncerLatencyLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove_all(directory_, error);
  }
  std::vector<std::string> Summaries() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    std::vector<std::string> result;
    for (std::string line; std::getline(file, line);) {
      if (line.find("stage=syncer.video ") != std::string::npos &&
          line.find("report=summary") != std::string::npos)
        result.push_back(line);
    }
    return result;
  }

 private:
  fs::path directory_;
  fs::path path_;
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime_;
};

AvDecoderReadResult Video() {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = frame->height = 8;
  frame->pts = 0;
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  result.time_base = {1, 25};
  return result;
}

SyncerConfig Config() {
  SyncerConfig config;
  config.encoder.video_pixel_format = AV_PIX_FMT_GRAY8;
  config.encoder.width = config.encoder.height = 8;
  config.encoder.fps = {25, 1};
  config.encoder.audio_sample_format = AV_SAMPLE_FMT_NONE;
  return config;
}

TEST_CASE("Syncer只统计成功输出且重置分隔统计会话", "[latency][syncer]") {
  SyncerLatencyLog log;
  {
    Syncer syncer(Config());
    auto input = Video();
    REQUIRE_FALSE(syncer.Step(true, nullptr, 0).frame);
    REQUIRE(syncer.Step(true, &input, 0, [](const auto&) { return false; })
                .blocked);
    REQUIRE(syncer.Step(true, &input, 0).frame);
    AvDecoderReadResult eof;
    eof.status = AvDecoderReadStatus::kEof;
    REQUIRE(syncer.Step(true, &eof, 1).consumed);
    // The shared clock extrapolates after two seconds without real input.
    REQUIRE(syncer.Step(true, nullptr, 3000000).frame);
    syncer.Reset();
    REQUIRE(syncer.Step(true, &input, 0).frame);
  }
  const auto summaries = log.Summaries();
  REQUIRE(summaries.size() == 2);
  CHECK(summaries[0].find("samples=2 unavailable=0") != std::string::npos);
  CHECK(summaries[1].find("samples=1 unavailable=0") != std::string::npos);
}

TEST_CASE("Syncer处理耗时排除下游回调耗时", "[latency][syncer]") {
  SyncerLatencyLog log;
  std::chrono::steady_clock::duration held{};
  std::chrono::steady_clock::duration elapsed{};
  {
    Syncer syncer(Config());
    auto input = Video();
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(syncer
                .Step(true, &input, 0,
                      [&](const auto&) {
                        const auto before = std::chrono::steady_clock::now();
                        std::this_thread::sleep_for(50ms);
                        held = std::chrono::steady_clock::now() - before;
                        return true;
                      })
                .frame);
    elapsed = std::chrono::steady_clock::now() - started;
  }
  const auto summaries = log.Summaries();
  REQUIRE(summaries.size() == 1);
  std::smatch match;
  REQUIRE(
      std::regex_search(summaries[0], match, std::regex(R"(max:([0-9.]+)ms)")));
  CHECK(std::stod(match[1].str()) <=
        std::chrono::duration<double, std::milli>(elapsed - held).count() +
            1.0);
}

}  // namespace

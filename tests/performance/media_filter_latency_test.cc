#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include "mw/log.h"
#include "mw/streamer/filter/av_media_filter.h"
#include "mw/streamer/init/init.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::AvMediaFilter;
using mw::streamer::PerformanceMode;
using mw::streamer::PerformanceStage;
namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;

class MediaFilterLatencyLog final {
 public:
  explicit MediaFilterLatencyLog(
      PerformanceMode video = PerformanceMode::kSummary,
      PerformanceMode audio = PerformanceMode::kSummary,
      bool configure_audio = true)
      : directory_(
            fs::temp_directory_path() /
            ("mw-media-filter-latency-" +
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
    config.log.async.enabled = false;
    config.log.rotating_file.enabled = true;
    config.log.rotating_file.path = path_.string();
    config.log.modules = {{"streamer", mw::log::LogLevel::kOff},
                          {"perf.filter.video", mw::log::LogLevel::kOff},
                          {"perf.filter.audio", mw::log::LogLevel::kTrace}};
    config.performance.stages = {{PerformanceStage::kMediaFilterVideo, video}};
    if (configure_audio) {
      config.performance.stages.emplace(PerformanceStage::kMediaFilterAudio,
                                        audio);
    }
    runtime_.reset(mw::streamer::Init(config));
  }

  ~MediaFilterLatencyLog() {
    runtime_.reset();
    std::error_code error;
    fs::remove_all(directory_, error);
  }

  std::string Read() {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), {}};
  }

  std::vector<std::string> Summaries(const std::string& stage) {
    runtime_.reset();
    std::ifstream file(path_);
    REQUIRE(file.is_open());
    std::vector<std::string> result;
    for (std::string line; std::getline(file, line);) {
      if (line.find("stage=" + stage + " ") != std::string::npos &&
          line.find("report=summary") != std::string::npos) {
        result.push_back(line);
      }
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

AvDecoderReadResult Input() {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_GRAY8;
  frame->width = frame->height = 8;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配业务过滤器 latency 测试帧失败");
  }
  frame->pts = 123;
  frame->time_base = {1, 1000};
  AvDecoderReadResult result;
  result.status = AvDecoderReadStatus::kFrame;
  result.frame = std::move(frame);
  return result;
}

std::size_t Samples(const std::string& line) {
  std::smatch match;
  REQUIRE(std::regex_search(line, match, std::regex("samples=([0-9]+)")));
  return std::stoull(match[1].str());
}

double MeanMs(const std::string& line) {
  std::smatch match;
  REQUIRE(
      std::regex_search(line, match, std::regex("latency\\(mean:([0-9.]+)ms")));
  return std::stod(match[1].str());
}

TEST_CASE("业务过滤器分别统计音视频回调且旁路和EOF不计样本",
          "[latency][media_filter]") {
  MediaFilterLatencyLog logging;
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& frame) {
    std::this_thread::sleep_for(12ms);
    return frame.Ref();
  });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) {
    std::this_thread::sleep_for(4ms);
    return frame.Ref();
  });
  filter.Start();
  filter.ProcessVideo(Input());
  filter.ProcessVideo(Input());
  filter.ProcessAudio(Input());
  AvDecoderReadResult eof;
  eof.status = AvDecoderReadStatus::kEof;
  filter.ProcessVideo(std::move(eof));
  filter.ProcessAudio({});
  filter.Stop();
  filter.Stop();
  filter.SetOnVideo({});
  filter.SetOnAudio({});
  filter.Start();
  filter.ProcessVideo(Input());
  filter.ProcessAudio(Input());
  filter.Stop();

  const auto video = logging.Summaries("filter.video");
  const auto audio = logging.Summaries("filter.audio");
  REQUIRE(video.size() == 2);
  REQUIRE(audio.size() == 2);
  CHECK(Samples(video[0]) == 2);
  CHECK(Samples(audio[0]) == 1);
  CHECK(MeanMs(video[0]) >= 10.0);
  CHECK(MeanMs(audio[0]) >= 3.0);
  CHECK(Samples(video[1]) == 0);
  CHECK(Samples(audio[1]) == 0);
}

TEST_CASE("业务过滤器失败调用不计成功样本且重启重新统计",
          "[latency][media_filter]") {
  MediaFilterLatencyLog logging;
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame&) -> ffmpeg::Frame {
    throw std::runtime_error("business failure");
  });
  filter.Start();
  REQUIRE_THROWS_AS(filter.ProcessVideo(Input()), std::runtime_error);
  REQUIRE(filter.error());
  filter.Stop();
  filter.SetOnVideo([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.Start();
  REQUIRE_FALSE(filter.error());
  filter.ProcessVideo(Input());
  filter.Stop();
  const auto video = logging.Summaries("filter.video");
  REQUIRE(video.size() == 2);
  CHECK(Samples(video[0]) == 0);
  CHECK(Samples(video[1]) == 1);
}

TEST_CASE("业务过滤器允许同轨并行并在Stop等待后汇总",
          "[latency][media_filter]") {
  MediaFilterLatencyLog logging;
  AvMediaFilter filter;
  std::promise<void> first_entered;
  std::promise<void> second_entered;
  auto first_ready = first_entered.get_future();
  auto second_ready = second_entered.get_future();
  std::promise<void> release;
  const auto released = release.get_future().share();
  filter.SetOnVideo([&](const ffmpeg::Frame& frame) {
    if (frame->pts == 1) {
      first_entered.set_value();
    } else {
      second_entered.set_value();
    }
    released.wait();
    return frame.Ref();
  });
  filter.Start();
  auto first = Input();
  (*first.frame)->pts = 1;
  auto call_first = std::async(std::launch::async, [&] {
    return filter.ProcessVideo(std::move(first));
  });
  auto call_second = std::async(std::launch::async,
                                [&] { return filter.ProcessVideo(Input()); });
  const auto first_status = first_ready.wait_for(1s);
  const auto second_status = second_ready.wait_for(1s);
  auto stop = std::async(std::launch::async, [&] { filter.Stop(); });
  const auto stop_status = stop.wait_for(20ms);
  release.set_value();
  REQUIRE_NOTHROW(call_first.get());
  REQUIRE_NOTHROW(call_second.get());
  REQUIRE_NOTHROW(stop.get());
  CHECK(first_status == std::future_status::ready);
  CHECK(second_status == std::future_status::ready);
  CHECK(stop_status == std::future_status::timeout);
  const auto video = logging.Summaries("filter.video");
  REQUIRE(video.size() == 1);
  CHECK(Samples(video[0]) == 2);
}

TEST_CASE("性能阶段独立于日志模块且分别控制汇总和明细",
          "[performance][latency][media_filter]") {
  MediaFilterLatencyLog logging(PerformanceMode::kDetail,
                                PerformanceMode::kSummary);
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.Start();
  filter.ProcessVideo(Input());
  filter.ProcessAudio(Input());
  filter.Stop();

  const auto log = logging.Read();
  CHECK(log.find("[perf.filter.video]") != std::string::npos);
  CHECK(log.find("[perf.filter.audio]") != std::string::npos);
  std::size_t video_details = 0;
  std::size_t audio_details = 0;
  std::size_t video_summaries = 0;
  std::size_t audio_summaries = 0;
  std::size_t begin = 0;
  while (begin < log.size()) {
    const auto end = log.find('\n', begin);
    const auto line = log.substr(begin, end - begin);
    const bool video = line.find("stage=filter.video ") != std::string::npos;
    const bool audio = line.find("stage=filter.audio ") != std::string::npos;
    if (line.find("report=summary") != std::string::npos && (video || audio)) {
      CHECK(line.find("[info]") != std::string::npos);
      CHECK(Samples(line) == 1);
      if (video) ++video_summaries;
      if (audio) ++audio_summaries;
    } else if (line.find("latency(") != std::string::npos && (video || audio)) {
      CHECK(line.find("[trace]") != std::string::npos);
      if (video) ++video_details;
      if (audio) ++audio_details;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  CHECK(video_summaries == 1);
  CHECK(audio_summaries == 1);
  CHECK(video_details == 1);
  CHECK(audio_details == 0);
}

TEST_CASE("Trace日志模块不能开启未配置的性能阶段",
          "[performance][latency][media_filter][disabled]") {
  MediaFilterLatencyLog logging(PerformanceMode::kSummary,
                                PerformanceMode::kOff, false);
  AvMediaFilter filter;
  filter.SetOnVideo([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.SetOnAudio([](const ffmpeg::Frame& frame) { return frame.Ref(); });
  filter.Start();
  filter.ProcessVideo(Input());
  filter.ProcessAudio(Input());
  filter.Stop();

  const auto log = logging.Read();
  CHECK(log.find("stage=filter.video ") != std::string::npos);
  CHECK(log.find("stage=filter.audio ") == std::string::npos);
}

}  // namespace

#include "mw/streamer/performance/performance.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>

#include "mw/log.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {
using namespace std::chrono_literals;
using mw::streamer::internal::PerformanceReport;
using mw::streamer::internal::PerformanceWindow;

TEST_CASE("性能日志默认可见且各模块独立控制", "[performance][logging]") {
  MwLogConfig config;
  mw_log_default_config(&config);
  config.console_enabled = 0;
  {
    mw::log::Logging logging(config);
    for (const auto* module :
         {"perf.input", "perf.decoder.video", "perf.decoder.audio",
          "perf.scheduler", "perf.encoder.video", "perf.encoder.audio"}) {
      CHECK(mw::log::ShouldLog(module, mw::log::LogLevel::kInfo));
      CHECK_FALSE(mw::log::ShouldLog(module, mw::log::LogLevel::kDebug));
    }
  }
  const std::string modules =
      "perf.input:off;perf.decoder.video:info;perf.decoder.audio:off;"
      "perf.scheduler:info;perf.encoder.video:off;perf.encoder.audio:info";
  config.modules = modules.c_str();
  config.modules_size = modules.size();
  mw::log::Logging logging(config);
  CHECK_FALSE(mw::log::ShouldLog("perf.input", mw::log::LogLevel::kInfo));
  CHECK(mw::log::ShouldLog("perf.decoder.video", mw::log::LogLevel::kInfo));
  CHECK_FALSE(
      mw::log::ShouldLog("perf.decoder.audio", mw::log::LogLevel::kInfo));
  CHECK(mw::log::ShouldLog("perf.scheduler", mw::log::LogLevel::kInfo));
  CHECK_FALSE(
      mw::log::ShouldLog("perf.encoder.video", mw::log::LogLevel::kInfo));
  CHECK(mw::log::ShouldLog("perf.encoder.audio", mw::log::LogLevel::kInfo));
}

TEST_CASE("性能窗口准确区分累计值、区间值和最大调用耗时", "[performance]") {
  PerformanceWindow window;
  PerformanceReport report;
  const PerformanceWindow::TimePoint start{};
  CHECK_FALSE(window.Sample(report, true, start));
  window.Reset(start);
  auto& counters = window.counters();
  counters.packets = 4;
  counters.bytes = 400;
  counters.frames = 3;
  counters.samples = 300;
  counters.media_ns = 40000000;
  counters.has_media = true;
  window.AddWork(100000);
  window.AddWork(500000);
  CHECK_FALSE(window.Sample(report, false, start + 1999ms));
  REQUIRE(window.Sample(report, false, start + 2s));
  CHECK(report.total.frames == 3);
  CHECK(report.interval.frames == 3);
  CHECK(report.elapsed_seconds == 2);
  CHECK(PerformanceReport::Rate(report.interval.frames,
                                report.interval_seconds) == 1.5);
  CHECK(report.interval.max_work_ns == 500000);

  counters.frames += 2;
  counters.samples += 200;
  counters.wait_ns += 3000000;
  ++counters.eof;
  ++counters.again;
  ++counters.reconnects;
  window.AddWork(250000);
  REQUIRE(window.Sample(report, false, start + 4s));
  CHECK(report.total.frames == 5);
  CHECK(report.interval.frames == 2);
  CHECK(report.total.work_calls == 3);
  CHECK(report.interval.work_calls == 1);
  CHECK(report.total.max_work_ns == 500000);
  CHECK(report.interval.max_work_ns == 250000);
  CHECK(report.interval.wait_ns == 3000000);
  CHECK(report.interval.eof == 1);
  CHECK(report.interval.again == 1);
  CHECK(report.interval.reconnects == 1);
}

TEST_CASE("短会话最终摘要去重且重启重新统计", "[performance]") {
  PerformanceWindow window;
  PerformanceReport report;
  const PerformanceWindow::TimePoint start{};
  window.Reset(start);
  window.counters().frames = 1;
  REQUIRE(window.Sample(report, true, start + 100ms));
  CHECK(report.total.frames == 1);
  CHECK_FALSE(window.Sample(report, true, start + 200ms));
  ++window.counters().errors;
  CHECK(window.Sample(report, true, start + 300ms));
  window.Reset(start + 1s);
  REQUIRE(window.Sample(report, true, start + 1s));
  CHECK(report.total.frames == 0);
  CHECK(report.total.errors == 0);
  CHECK(PerformanceReport::Rate(1, 0) == 0);
}

std::uint64_t Field(const std::string& text, const std::string& module,
                    const std::string& field) {
  const auto escaped_module =
      std::regex_replace(module, std::regex("\\."), "\\.");
  const std::regex expression("\\[" + escaped_module + "\\][^\\r\\n]*\\b" +
                              field + "=([0-9]+)");
  std::uint64_t value = 0;
  bool found = false;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), expression);
       it != std::sregex_iterator(); ++it) {
    value = std::stoull((*it)[1]);
    found = true;
  }
  CHECK(found);
  return value;
}

TEST_CASE("实际Input解码调度日志匹配数据且模块可以独立关闭",
          "[performance][input]") {
  const std::string level = GENERATE("info", "trace", "off");
  const bool enabled = level != "off";
  const auto log_path =
      std::filesystem::temp_directory_path() /
      ("mw-perf-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".log");
  const auto log_name = log_path.string();
  mw::streamer::InitConfig config;
  config.event_poller_threads = 2;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console_enabled = 0;
  config.log.rotating_file_enabled = 1;
  config.log.rotating_file_path = log_name.c_str();
  config.log.rotating_file_path_size = log_name.size();
  const std::string modules =
      "perf.input:" + level + ";perf.decoder.video:" + level +
      ";perf.decoder.audio:" + level + ";perf.scheduler:" + level;
  config.log.modules = modules.c_str();
  config.log.modules_size = modules.size();
  std::unique_ptr<mw::streamer::MwStreamerContext,
                  decltype(&mw::streamer::Shutdown)>
      runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
  std::atomic<std::uint64_t> packets{0}, bytes{0}, video_frames{0},
      audio_frames{0}, samples{0}, output_samples{0};
  bool ended = false;
  bool failed = false;
  std::mutex mutex;
  std::condition_variable changed;
  {
    mw::streamer::Scheduler scheduler;
    scheduler.SetOnAudio([&](const auto& frame) noexcept {
      output_samples += frame->nb_samples;
    });
    mw::streamer::FfmpegInputConfig input_config;
    input_config.auto_reconnect = false;
    mw::streamer::FfmpegInput input(input_config);
    scheduler.SetOnEnded([&]() noexcept {
      std::lock_guard<std::mutex> lock(mutex);
      ended = true;
      changed.notify_all();
    });
    input.SetOnReady([&](const auto& streams) noexcept {
      if (!scheduler.Start(streams)) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        changed.notify_all();
      }
    });
    input.SetOnPacket([&](std::uint64_t, const auto& packet) noexcept {
      ++packets;
      bytes += packet->size;
    });
    input.SetOnFrame([&](int, const auto& frame) noexcept {
      if (frame->width > 0) {
        ++video_frames;
        scheduler.SubmitVideo(frame);
      } else {
        ++audio_frames;
        samples += frame->nb_samples;
        scheduler.SubmitAudio(frame);
      }
    });
    input.SetOnStateChanged([&](auto state, int, std::string_view) noexcept {
      if (state == mw::streamer::InputState::kEnded) scheduler.Drain();
      if (state == mw::streamer::InputState::kFailed) {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        changed.notify_all();
      }
    });
    input.Start(std::string(MW_STREAMER_PERFORMANCE_TEST_DATA_DIR) +
                "/h264_aac.mp4");
    bool finished;
    {
      std::unique_lock<std::mutex> lock(mutex);
      finished = changed.wait_for(lock, 8s, [&] { return ended || failed; });
    }
    input.Stop();
    scheduler.Stop();
    CHECK(finished);
    CHECK_FALSE(failed);
    CHECK(ended);
  }
  // Synchronous logging is flushed when the runtime and its logger close.
  runtime.reset();
  std::ifstream file(log_path);
  const std::string text((std::istreambuf_iterator<char>(file)), {});
  if (enabled) {
    std::istringstream lines(text);
    std::string line;
    std::string overview;
    while (std::getline(lines, line)) {
      if (line.find("[info]") == std::string::npos ||
          line.find("[perf.") == std::string::npos)
        continue;
      overview += line;
      // INFO keeps the operational overview; accounting details belong to
      // TRACE.
      CHECK(line.find("total_packets=") == std::string::npos);
      CHECK(line.find("api_work_calls=") == std::string::npos);
      CHECK(line.find("consumed_source_samples=") == std::string::npos);
      CHECK(line.find("late_tick_threshold_ms=") == std::string::npos);
      CHECK(line.find("playback_wait_ms=") == std::string::npos);
      CHECK(line.find("eagain=") == std::string::npos);
    }
    for (const auto* module : {"perf.input", "perf.decoder.video",
                               "perf.decoder.audio", "perf.scheduler"}) {
      CHECK(overview.find(std::string("[") + module + "]") !=
            std::string::npos);
    }
    CHECK(overview.find("report=summary") != std::string::npos);
    CHECK(text.find("nan") == std::string::npos);
    if (level == "trace") {
      CHECK(text.find("[trace]") != std::string::npos);
      CHECK(Field(text, "perf.input", "total_packets") == packets.load());
      CHECK(Field(text, "perf.input", "total_payload_bytes") == bytes.load());
      CHECK(Field(text, "perf.decoder.video", "frames") == video_frames.load());
      CHECK(Field(text, "perf.decoder.audio", "frames") == audio_frames.load());
      CHECK(Field(text, "perf.decoder.audio", "samples") == samples.load());
      CHECK(Field(text, "perf.scheduler", "delivered_samples") ==
            output_samples.load());
      CHECK(Field(text, "perf.scheduler", "delivered_source_samples") +
                Field(text, "perf.scheduler", "zero_fill_samples") ==
            output_samples.load());
    } else {
      CHECK(text.find("[trace]") == std::string::npos);
    }
  } else {
    CHECK(text.find("[perf.") == std::string::npos);
  }
  file.close();
  std::filesystem::remove(log_path);
}
}  // namespace

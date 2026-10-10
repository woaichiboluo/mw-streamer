#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/error.h>
}

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/remux_pipeline.h"

namespace {

namespace streamer = mw::streamer;
using Clock = std::chrono::steady_clock;

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "Signal handling requires lock-free atomic stores");

void RequestStop(int) { stop_requested.store(true, std::memory_order_relaxed); }

const char* StateName(streamer::PipelineState state) {
  switch (state) {
    case streamer::PipelineState::kIdle:
      return "未启动";
    case streamer::PipelineState::kStarting:
      return "启动中";
    case streamer::PipelineState::kRunning:
      return "运行中";
    case streamer::PipelineState::kReconnecting:
      return "重连中";
    case streamer::PipelineState::kStopping:
      return "停止中";
    case streamer::PipelineState::kStopped:
      return "已停止";
    case streamer::PipelineState::kEnded:
      return "输入结束";
    case streamer::PipelineState::kFailed:
      return "失败";
  }
  return "未知";
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"RemuxPipeline 示例：原包转推、录制和本地发布"};
  argv = app.ensure_utf8(argv);
  std::string input;
  std::vector<std::string> push_urls;
  std::vector<std::string> input_options;
  bool rtsp = false, hls = false, loop = false;
  int duration_seconds = 0;
  app.add_option("--input", input, "输入地址或本地文件")->required();
  app.add_option("--input-option", input_options,
                 "FFmpeg 输入选项 key=value，可重复；同名选项后者覆盖")
      ->expected(1)
      ->take_all()
      ->check(CLI::Validator(
          [](std::string& value) {
            const auto separator = value.find('=');
            return separator == std::string::npos || separator == 0
                       ? std::string("输入选项必须为 key=value")
                       : std::string();
          },
          "KEY=VALUE"));
  auto* outputs = app.add_option_group("输出", "至少开启一种输出");
  outputs
      ->add_option("--push", push_urls, "推流地址或 MP4/M3U8 录制路径，可重复")
      ->expected(1)
      ->take_all();
  outputs->add_flag("--rtsp", rtsp, "开启本地 RTSP 发布（8554）")
      ->disable_flag_override();
  outputs->add_flag("--hls", hls, "开启本地 HLS 发布（8080）")
      ->disable_flag_override();
  outputs->require_option(1, 0);
  app.add_flag("--loop", loop, "文件输入到 EOF 后循环");
  app.add_option("--duration", duration_seconds, "启动后运行指定秒数再停止")
      ->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);

  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);
  try {
    streamer::InitConfig init_config;
    init_config.event_poller_threads = 2;
    init_config.work_threads = 1;
    init_config.enable_cpu_affinity = false;
    const std::unique_ptr<streamer::MwStreamerContext,
                          decltype(&streamer::Shutdown)>
        runtime(streamer::Init(init_config), &streamer::Shutdown);
    std::mutex output_mutex;
    streamer::RemuxPipelineConfig config;
    config.input.loop = loop;
    for (const auto& option : input_options) {
      const auto separator = option.find('=');
      config.input.options[option.substr(0, separator)] =
          option.substr(separator + 1);
    }
    // Pipeline is destroyed before runtime, including on exceptions.
    streamer::RemuxPipeline pipeline(config);
    pipeline.SetOnStateChanged([&](const streamer::PipelineStatus& status) {
      std::lock_guard lock(output_mutex);
      std::cout << "状态：" << StateName(status.state);
      if (!status.error_message.empty()) {
        std::cout << "，" << status.error_source << "："
                  << status.error_message;
      }
      std::cout << std::endl;
    });
    pipeline.SetOnReady([&](const streamer::RemuxPipelineReadyInfo& info) {
      std::lock_guard lock(output_mutex);
      std::cout << "输入：" << input << "，轨道数：" << info.streams.size()
                << '\n';
      for (const auto& url : info.output_urls)
        std::cout << "输出：" << url << '\n';
      if (!info.rtsp_url.empty())
        std::cout << "RTSP：" << info.rtsp_url << '\n';
      if (!info.hls_url.empty()) std::cout << "HLS：" << info.hls_url << '\n';
      std::cout << "按 Ctrl+C 停止。" << std::endl;
    });

    // Demonstrate output registration before Start; runtime additions use the
    // same methods. AddPushUrl's actual recording path is printed by OnReady.
    for (const auto& url : push_urls) pipeline.AddPushUrl(url);
    if (rtsp) pipeline.AddRtspPublish("live", "demo", "127.0.0.1", 8554);
    if (hls) pipeline.AddHlsPublish("live", "demo", "127.0.0.1", 8080);
    // Start waits through connection retries. Keep the calling thread able to
    // request Stop while it is still connecting; the future joins that call.
    auto startup =
        std::async(std::launch::async, [&] { pipeline.Start(input); });
    while (startup.wait_for(std::chrono::milliseconds(100)) !=
           std::future_status::ready) {
      if (stop_requested.load(std::memory_order_relaxed) &&
          pipeline.state() != streamer::PipelineState::kIdle) {
        pipeline.Stop();
      }
    }
    try {
      startup.get();
    } catch (const streamer::ffmpeg::FfmpegException& error) {
      if (error.error_code() == AVERROR_EXIT &&
          stop_requested.load(std::memory_order_relaxed) &&
          pipeline.state() == streamer::PipelineState::kStopped) {
        return 0;
      }
      throw;
    }

    const auto started = Clock::now();
    while (!stop_requested.load(std::memory_order_relaxed)) {
      const auto state = pipeline.state();
      if (state == streamer::PipelineState::kEnded ||
          state == streamer::PipelineState::kFailed ||
          state == streamer::PipelineState::kStopped) {
        break;
      }
      if (duration_seconds > 0 &&
          Clock::now() - started >= std::chrono::seconds(duration_seconds)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Stop runs on the calling thread, never inside a pipeline callback. It
    // waits for accepted packets and finalizes recordings before Shutdown.
    pipeline.Stop();
    return pipeline.state() == streamer::PipelineState::kFailed ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "运行失败：" << error.what() << '\n';
    return 1;
  }
}

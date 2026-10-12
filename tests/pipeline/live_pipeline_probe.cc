#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/live_pipeline.h"

namespace {

using Clock = std::chrono::steady_clock;
using mw::streamer::PipelineState;

struct Track {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::int64_t> first{0}, end{0};
  std::atomic<int> sample_rate{0};
  std::atomic<double> time_base{0};

  void Consume(const mw::streamer::ffmpeg::Frame& frame) noexcept {
    if (frames.fetch_add(1) == 0) first = frame->pts;
    end = frame->pts + frame->duration;
    time_base = av_q2d(frame->time_base);
    if (frame->sample_rate > 0) sample_rate = frame->sample_rate;
  }
  double MediaSeconds() const noexcept {
    return frames.load() ? static_cast<double>(end.load() - first.load()) *
                               time_base.load()
                         : 0.0;
  }
};

const char* StateName(PipelineState state) {
  switch (state) {
    case PipelineState::kIdle:
      return "idle";
    case PipelineState::kStarting:
      return "starting";
    case PipelineState::kRunning:
      return "running";
    case PipelineState::kReconnecting:
      return "reconnecting";
    case PipelineState::kStopping:
      return "stopping";
    case PipelineState::kStopped:
      return "stopped";
    case PipelineState::kEnded:
      return "ended";
    case PipelineState::kFailed:
      return "failed";
  }
  return "unknown";
}

}  // namespace

// Manual recording through the public LivePipeline/AddPushUrl interfaces.
int main(int argc, char** argv) {
  if (argc < 3) {
    fmt::print(stderr,
               "Usage: {} INPUT OUTPUT [WALL_SECONDS=120] [WIDTH=7680] "
               "[HEIGHT=2160] [FPS=50] [cpu|cuda] [loop|once] [USER_AGENT] "
               "[INPUT_OPTION=VALUE ...]\n"
               "WALL_SECONDS=0 waits for normal EOF.\n",
               argv[0]);
    return 2;
  }
  try {
    const double limit = argc > 3 ? std::stod(argv[3]) : 120.0;
    const std::string backend = argc > 7 ? argv[7] : "cuda";
    const std::string mode = argc > 8 ? argv[8] : "loop";
    if (limit < 0 || (backend != "cpu" && backend != "cuda") ||
        (mode != "loop" && mode != "once"))
      throw std::invalid_argument("Invalid duration or device");
    const bool cuda = backend == "cuda";
    mw::streamer::LivePipelineConfig config;
    config.input.loop = mode == "loop";
    if (argc > 9 && argv[9][0]) config.input.options["user_agent"] = argv[9];
    for (int i = 10; i < argc; ++i) {
      const std::string option = argv[i];
      const auto separator = option.find('=');
      if (separator == std::string::npos || separator == 0)
        throw std::invalid_argument("Input option must use KEY=VALUE");
      config.input.options[option.substr(0, separator)] =
          option.substr(separator + 1);
    }
    config.encoder.width = argc > 4 ? std::stoi(argv[4]) : 7680;
    config.encoder.height = argc > 5 ? std::stoi(argv[5]) : 2160;
    config.encoder.fps = {argc > 6 ? std::stoi(argv[6]) : 50, 1};
    config.scheduler.video_frame_rate = config.encoder.fps;
    config.encoder.video_pixel_format =
        cuda ? AV_PIX_FMT_NV12 : AV_PIX_FMT_NONE;
    config.encoder.video_encoder_name = cuda ? "hevc_nvenc" : "libx264";
    config.encoder.video_bit_rate = 30000000;
    config.encoder.gop_size = config.encoder.fps.num * 2;
    config.encoder.video_options =
        cuda ? std::map<std::string, std::string>{{"preset", "p2"}}
             : std::map<std::string, std::string>{{"preset", "ultrafast"},
                                                  {"tune", "zerolatency"}};
    mw::streamer::InitConfig init_config;
    init_config.event_poller_threads = 2;
    init_config.work_threads = 1;
    init_config.enable_cpu_affinity = false;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(init_config), &mw::streamer::Shutdown);
    const mw::streamer::ffmpeg::HwDeviceContext device(
        cuda ? mw::streamer::ffmpeg::HwDeviceType::kCuda
             : mw::streamer::ffmpeg::HwDeviceType::kCpu);
    mw::streamer::LivePipeline pipeline(device, config);
    Track video, audio;
    std::atomic<int> ready{0}, ended{0}, stopped{0}, invalid{0};
    std::atomic<int> order{0}, end_order{0}, stop_order{0};
    std::atomic<std::uint64_t> cuda_frames{0};
    pipeline.SetOnReady([&](const auto&, const auto& actual_device) noexcept {
      ++ready;
      if (cuda && (!actual_device.get() ||
                   actual_device.get()->data != device.get()->data))
        ++invalid;
      return invalid.load() == 0;
    });
    pipeline.SetOnVideo([&](const auto& frame) noexcept {
      video.Consume(frame);
      if (frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx)
        ++cuda_frames;
      return frame.Ref();
    });
    pipeline.SetOnAudio([&](const auto& frame) noexcept {
      audio.Consume(frame);
      return frame.Ref();
    });
    pipeline.SetOnEnded([&]() noexcept {
      ++ended;
      end_order = ++order;
    });
    pipeline.SetOnStop([&]() noexcept {
      ++stopped;
      stop_order = ++order;
    });
    const auto output = pipeline.AddPushUrl(argv[2]);
    fmt::print("OUTPUT {}\n", output);
    std::fflush(stdout);
    const auto started = Clock::now();
    pipeline.Start(argv[1]);
    const double startup =
        std::chrono::duration<double>(Clock::now() - started).count();
    const auto processing = Clock::now();
    auto next_report = processing;
    while (pipeline.state() == PipelineState::kRunning ||
           pipeline.state() == PipelineState::kReconnecting) {
      const auto now = Clock::now();
      const double wall =
          std::chrono::duration<double>(now - processing).count();
      if (limit > 0 && wall >= limit) break;
      if (now >= next_report) {
        fmt::print(
            "PROGRESS wall={:.3f} state={} video={} audio={} "
            "video_media={:.3f} audio_media={:.3f}\n",
            wall, StateName(pipeline.state()), video.frames.load(),
            audio.frames.load(), video.MediaSeconds(), audio.MediaSeconds());
        std::fflush(stdout);
        next_report = now + std::chrono::seconds(10);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stopping = Clock::now();
    pipeline.Stop();
    const double stop_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - stopping)
            .count();
    const double processed =
        std::chrono::duration<double>(Clock::now() - processing).count();
    const auto status = pipeline.status();
    fmt::print(
        "RESULT state={} ready={} ended={} stopped={} "
        "startup_seconds={:.6f} processing_seconds={:.6f} stop_ms={:.3f} "
        "video_frames={} audio_frames={} video_media_seconds={:.6f} "
        "audio_media_seconds={:.6f} video_fps={:.3f} audio_sample_rate={} "
        "cuda_frames={} backend={} loop={} "
        "end_order={} stop_order={}\n",
        StateName(status.state), ready.load(), ended.load(), stopped.load(),
        startup, processed, stop_ms, video.frames.load(), audio.frames.load(),
        video.MediaSeconds(), audio.MediaSeconds(),
        static_cast<double>(video.frames.load()) / processed,
        audio.sample_rate.load(), cuda_frames.load(), backend,
        config.input.loop, end_order.load(), stop_order.load());
    if (status.state == PipelineState::kFailed || invalid.load() ||
        (cuda && cuda_frames.load() != video.frames.load())) {
      fmt::print(stderr, "{}: {}\n", status.error_source, status.error_message);
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "LivePipeline probe: {}\n", error.what());
    return 1;
  }
}

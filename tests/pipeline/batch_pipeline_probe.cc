#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <thread>

extern "C" {
#include <libavutil/mathematics.h>
}

#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/batch_pipeline.h"

namespace {

using Clock = std::chrono::steady_clock;
using mw::streamer::PipelineState;
constexpr auto kUnset = std::numeric_limits<std::int64_t>::min();

struct Track {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::int64_t> first{kUnset}, end{kUnset};
  std::atomic<int> sample_rate{0};

  void Consume(const mw::streamer::ffmpeg::Frame& frame) noexcept {
    frames.fetch_add(1);
    if (frame->pts != AV_NOPTS_VALUE && frame->time_base.num > 0 &&
        frame->time_base.den > 0) {
      constexpr AVRational kNanoseconds{1, 1000000000};
      const auto pts = av_rescale_q(frame->pts, frame->time_base, kNanoseconds);
      auto unset = kUnset;
      first.compare_exchange_strong(unset, pts);
      end.store(pts +
                av_rescale_q(frame->duration, frame->time_base, kNanoseconds));
    }
    if (frame->sample_rate > 0) sample_rate.store(frame->sample_rate);
  }

  double MediaSeconds() const noexcept {
    return first.load() != kUnset && end.load() != kUnset
               ? static_cast<double>(end.load() - first.load()) / 1e9
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

// Manual real-input measurement; zero wall limit runs through normal EOF.
int main(int argc, char** argv) {
  if (argc < 2 || argc > 6) {
    fmt::print(stderr,
               "Usage: {} INPUT [WALL_SECONDS=0] [SPEED=8] [USER_AGENT] "
               "[cpu|cuda]\n",
               argv[0]);
    return 2;
  }
  try {
    const double wall_limit = argc > 2 ? std::stod(argv[2]) : 0.0;
    const std::string backend = argc > 5 ? argv[5] : "cpu";
    if (backend != "cpu" && backend != "cuda") {
      fmt::print(stderr, "Unknown device: {}\n", backend);
      return 2;
    }
    const bool cuda = backend == "cuda";
    mw::streamer::BatchPipelineConfig config;
    if (argc > 3) config.playback_speed = std::stod(argv[3]);
    if (argc > 4 && argv[4][0]) config.input.options["user_agent"] = argv[4];
    config.input.auto_reconnect = false;
    mw::streamer::InitConfig init_config;
    init_config.event_poller_threads = 2;
    init_config.work_threads = 1;
    init_config.enable_cpu_affinity = false;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(init_config), &mw::streamer::Shutdown);
    Track video, audio;
    std::atomic<int> ready{0}, ended{0}, stopped{0}, order{0};
    std::atomic<int> end_order{0}, stop_order{0};
    std::atomic<std::uint64_t> cuda_frames{0};
    std::atomic<bool> ready_cuda{false};
    const mw::streamer::ffmpeg::HwDeviceContext device(
        cuda ? mw::streamer::ffmpeg::HwDeviceType::kCuda
             : mw::streamer::ffmpeg::HwDeviceType::kCpu);
    mw::streamer::BatchPipeline pipeline(device, config);
    pipeline.SetOnReady([&](const auto&, const auto& ready_device) noexcept {
      ++ready;
      if (ready_device.get()) {
        ready_cuda.store(
            reinterpret_cast<const AVHWDeviceContext*>(ready_device.get()->data)
                ->type == AV_HWDEVICE_TYPE_CUDA);
      }
      return true;
    });
    pipeline.SetOnVideo([&](const auto& frame) noexcept {
      video.Consume(frame);
      if (frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx)
        ++cuda_frames;
    });
    pipeline.SetOnAudio(
        [&](const auto& frame) noexcept { audio.Consume(frame); });
    pipeline.SetOnEnded([&]() noexcept {
      ++ended;
      end_order.store(++order);
    });
    pipeline.SetOnStop([&]() noexcept {
      ++stopped;
      stop_order.store(++order);
    });
    const auto started = Clock::now();
    pipeline.Start(argv[1]);
    const double startup_seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    const auto processing_started = Clock::now();
    auto next_report = processing_started;
    for (;;) {
      const auto state = pipeline.state();
      if (state == PipelineState::kEnded || state == PipelineState::kFailed ||
          state == PipelineState::kStopped)
        break;
      const auto now = Clock::now();
      const double wall =
          std::chrono::duration<double>(now - processing_started).count();
      if (wall_limit > 0 && wall >= wall_limit) break;
      if (now >= next_report) {
        fmt::print(
            "PROGRESS wall={:.3f} video={} audio={} video_media={:.3f} "
            "audio_media={:.3f}\n",
            wall, video.frames.load(), audio.frames.load(),
            video.MediaSeconds(), audio.MediaSeconds());
        std::fflush(stdout);
        next_report = now + std::chrono::seconds(5);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stopping = Clock::now();
    pipeline.Stop();
    const double stop_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - stopping)
            .count();
    const double wall =
        std::chrono::duration<double>(Clock::now() - started).count();
    const double processed =
        std::chrono::duration<double>(Clock::now() - processing_started)
            .count();
    const auto status = pipeline.status();
    fmt::print(
        "RESULT state={} ready={} ended={} stopped={} end_order={} "
        "stop_order={} startup_seconds={:.6f} wall_seconds={:.6f} "
        "processing_seconds={:.6f} stop_ms={:.3f} video_frames={} "
        "audio_frames={} video_media_seconds={:.6f} audio_media_seconds={:.6f} "
        "video_fps={:.3f} speed={:.3f} audio_sample_rate={} "
        "configured_speed={} backend={} ready_cuda={} cuda_frames={}\n",
        StateName(status.state), ready.load(), ended.load(), stopped.load(),
        end_order.load(), stop_order.load(), startup_seconds, wall, processed,
        stop_ms, video.frames.load(), audio.frames.load(), video.MediaSeconds(),
        audio.MediaSeconds(),
        static_cast<double>(video.frames.load()) / processed,
        video.MediaSeconds() / processed, audio.sample_rate.load(),
        config.playback_speed, backend, ready_cuda.load(), cuda_frames.load());
    if (cuda && (!ready_cuda.load() || video.frames.load() == 0 ||
                 cuda_frames.load() != video.frames.load())) {
      fmt::print(stderr, "CUDA decoding was not used for every video frame\n");
      return 1;
    }
    if (status.state == PipelineState::kFailed) {
      fmt::print(stderr, "{}: {}\n", status.error_source, status.error_message);
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "BatchPipeline probe: {}\n", error.what());
    return 1;
  }
}

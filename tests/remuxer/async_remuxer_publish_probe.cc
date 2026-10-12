#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "mw/streamer/init/init.h"
#include "mw/streamer/pipeline/live_pipeline.h"

namespace {
using namespace std::chrono_literals;

mw::streamer::LivePipelineConfig Config() {
  mw::streamer::LivePipelineConfig config;
  config.encoder.gop_size = 60;
  config.encoder.video_encoder_name = "libx264";
  config.encoder.audio_encoder_name = "aac";
  config.encoder.video_options = {{"preset", "veryfast"},
                                  {"tune", "zerolatency"}};
  return config;
}

class Publishing final {
 public:
  Publishing() : pipeline_(Config()) {
    pipeline_.SetOnVideo([this](const auto& frame) noexcept {
      ++videos_;
      return frame.Ref();
    });
    pipeline_.SetOnAudio([this](const auto& frame) noexcept {
      ++audios_;
      return frame.Ref();
    });
  }

  void Run(const std::string& input_url, std::uint16_t port,
           const std::filesystem::path& stop_file) {
    pipeline_.AddRtspPublish("live", "tencent", "0.0.0.0", port);
    pipeline_.Start(input_url);
    fmt::print("READY rtsp://127.0.0.1:{}/live/tencent\n", port);
    std::fflush(stdout);
    while (!std::filesystem::exists(stop_file)) {
      const auto state = pipeline_.state();
      if (state == mw::streamer::PipelineState::kFailed ||
          state == mw::streamer::PipelineState::kEnded ||
          state == mw::streamer::PipelineState::kStopped)
        break;
      std::this_thread::sleep_for(250ms);
    }
    pipeline_.Stop();
    const auto status = pipeline_.status();
    if (status.state == mw::streamer::PipelineState::kFailed)
      throw std::runtime_error(status.error_source + ": " +
                               status.error_message);
    fmt::print("STOPPED video_frames={} audio_frames={}\n", videos_.load(),
               audios_.load());
  }

 private:
  mw::streamer::LivePipeline pipeline_;
  std::atomic<std::uint64_t> videos_{0}, audios_{0};
};
}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    fmt::print(stderr, "Usage: {} input_url rtsp_port stop_file\n", argv[0]);
    return 2;
  }
  try {
    const auto port = std::stoi(argv[2]);
    if (port <= 0 || port > 65535)
      throw std::invalid_argument("RTSP 端口必须在 1 至 65535 之间");
    mw::streamer::InitConfig config;
    config.event_poller_threads = 2;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    const std::unique_ptr<mw::streamer::MwStreamerContext,
                          decltype(&mw::streamer::Shutdown)>
        runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
    Publishing publishing;
    publishing.Run(argv[1], static_cast<std::uint16_t>(port),
                   std::filesystem::u8path(argv[3]));
    return 0;
  } catch (const std::exception& error) {
    fmt::print(stderr, "发布探针失败: {}\n", error.what());
    return 1;
  }
}

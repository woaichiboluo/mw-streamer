#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

#include "mw/streamer/input/ffmpeg_input.h"
#include "scheduler_test_support.h"

namespace {
using namespace mw::streamer::testing;

using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputState;

void InputIntegration(bool cuda) {
  bool ready = false;
  bool valid_gpu = true;
  int filtered = 0;
  int gpu_outputs = 0;
  std::optional<ffmpeg::Frame> retained;
  std::optional<ffmpeg::HwDeviceContext> device;
  device.emplace(cuda ? ffmpeg::HwDeviceType::kCuda
                      : ffmpeg::HwDeviceType::kCpu);
  Rig rig;
  rig.processor.SetOnVideo([&](const ffmpeg::Frame& frame) {
    return rig.errors.Filter([&] {
      std::lock_guard<std::mutex> lock(rig.mutex);
      ++filtered;
      if (cuda) {
        valid_gpu &= frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx;
        if (frame->hw_frames_ctx) {
          const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
              frame->hw_frames_ctx->data);
          valid_gpu &=
              pool->device_ctx ==
              reinterpret_cast<const AVHWDeviceContext*>(device->get()->data);
        }
        retained = frame.Ref();
      }
      return frame.Ref();
    });
  });
  if (cuda) {
    rig.SetVideoSink([&](const ffmpeg::Frame& frame) {
      rig.errors.Run([&] {
        std::lock_guard<std::mutex> lock(rig.mutex);
        ++gpu_outputs;
        valid_gpu &= frame->format == AV_PIX_FMT_CUDA && frame->hw_frames_ctx;
      });
    });
  }
  FfmpegInputConfig config;
  config.auto_reconnect = false;
  FfmpegInput input(*device, config);
  input.SetOnReady([&](const auto& streams) {
    rig.errors.Run([&] {
      ready =
          rig.processor.Start(streams, *device) && rig.scheduler.Start(streams);
    });
  });
  input.SetOnFrame([&](int, const ffmpeg::Frame& frame) {
    rig.errors.Run([&] {
      if (frame->width > 0) {
        rig.scheduler.SubmitVideo(frame);
      } else {
        rig.scheduler.SubmitAudio(frame);
      }
    });
  });
  input.SetOnStateChanged([&](InputState state, int, std::string_view) {
    if (state == InputState::kEnded) rig.scheduler.Drain();
  });
  input.Start(std::string(MW_STREAMER_SCHEDULER_TEST_DATA_DIR) +
              (cuda ? "/h265_cuda.mp4" : "/h264_aac.mp4"));
  const bool ended = rig.Wait([&] { return rig.ended == 1; }, 8s);
  input.Stop();
  rig.Finish();
  CHECK(ended);
  CHECK(ready);
  CHECK(filtered > 0);
  if (cuda) {
    CHECK(valid_gpu);
    CHECK(gpu_outputs > 0);
    REQUIRE(retained.has_value());
    device.reset();
    CHECK((*retained)->format == AV_PIX_FMT_CUDA);
    CHECK((*retained)->hw_frames_ctx != nullptr);
  } else {
    CHECK_FALSE(rig.videos.empty());
    CHECK_FALSE(rig.audios.empty());
  }
}

TEST_CASE("本地Input连接Scheduler与Processor并排空音视频",
          "[scheduler][input]") {
  InputIntegration(false);
}

TEST_CASE("CUDA Input经Scheduler与Processor保留GPU帧与外部设备",
          "[.][scheduler][cuda]") {
  InputIntegration(true);
}

}  // namespace

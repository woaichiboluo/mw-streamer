#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "mw/streamer/pipeline/live_pipeline.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/init/init.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::PipelineState;
using namespace std::chrono_literals;

class CudaRuntime final {
 public:
  CudaRuntime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    config.log.console_enabled = 0;
    context_ = mw::streamer::Init(config);
  }
  ~CudaRuntime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_ = nullptr;
};

class CudaRecording final {
 public:
  CudaRecording()
      : directory_(
            std::filesystem::current_path() /
            ("live-cuda-" + std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()))) {
    std::filesystem::create_directory(directory_);
  }
  ~CudaRecording() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  std::string path() const {
    return (directory_ / "record.mp4").generic_u8string();
  }

 private:
  std::filesystem::path directory_;
};

int DecodeCudaRecording(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开 CUDA 管线录像");
  const auto close = [](AVFormatContext* context) {
    avformat_close_input(&context);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "读取 CUDA 管线录像轨道");
  const int index =
      av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  ffmpeg::CodecContext decoder(
      avcodec_find_decoder(raw->streams[index]->codecpar->codec_id));
  ffmpeg::FfmpegException::throwIfError(
      avcodec_parameters_to_context(decoder.get(),
                                    raw->streams[index]->codecpar),
      "配置 CUDA 管线录像解码器");
  ffmpeg::FfmpegException::throwIfError(
      avcodec_open2(decoder.get(), nullptr, nullptr),
      "打开 CUDA 管线录像解码器");
  int frames = 0;
  const auto receive = [&] {
    ffmpeg::Frame frame;
    for (;;) {
      const int result = avcodec_receive_frame(decoder.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(result, "解码 CUDA 管线录像");
      CHECK(frame->width == 256);
      CHECK(frame->height == 144);
      CHECK(frame->data[0] != nullptr);
      ++frames;
    }
  };
  ffmpeg::Packet packet;
  for (;;) {
    const int result = av_read_frame(raw, packet.get());
    if (result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "读取 CUDA 管线录像包");
    if (packet->stream_index == index) {
      ffmpeg::FfmpegException::throwIfError(
          avcodec_send_packet(decoder.get(), packet.get()),
          "提交 CUDA 管线录像解码包");
      receive();
    }
    packet.Unref();
  }
  ffmpeg::FfmpegException::throwIfError(
      avcodec_send_packet(decoder.get(), nullptr), "排空 CUDA 管线录像解码器");
  receive();
  return frames;
}

TEST_CASE(
    "LivePipeline preserves CUDA surfaces through NVENC and EOF recording",
    "[.][pipeline][live][cuda]") {
  const std::string encoder = GENERATE("h264_nvenc", "hevc_nvenc");
  CAPTURE(encoder);
  CudaRuntime runtime;
  // This explicit hardware test fails when CUDA creation is unavailable.
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  CudaRecording recording;
  mw::streamer::LivePipelineConfig config;
  config.input.auto_reconnect = false;
  config.scheduler.video_frame_rate = {30, 1};
  config.encoder.fps = {30, 1};
  config.encoder.width = 256;
  config.encoder.height = 144;
  config.encoder.video_pixel_format = AV_PIX_FMT_NV12;
  config.encoder.video_encoder_name = encoder;
  config.encoder.max_b_frames = 2;
  mw::streamer::LivePipeline pipeline(device, config);
  std::atomic<int> ready{0}, video{0}, events{0}, ended{0}, stopped{0};
  std::atomic<bool> invalid_surface{false}, wrong_device{false};
  pipeline.SetOnReady([&](const auto& streams, const auto& actual_device) {
    ++ready;
    wrong_device = streams.empty() || !actual_device.get() ||
                   actual_device.get()->data != device.get()->data;
    return !wrong_device;
  });
  pipeline.SetOnVideo([&](const ffmpeg::Frame& frame) {
    ++video;
    if (frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx) {
      invalid_surface = true;
    } else {
      const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
          frame->hw_frames_ctx->data);
      if (frames->sw_format != AV_PIX_FMT_NV12 || !frames->device_ref ||
          frames->device_ref->data != device.get()->data)
        invalid_surface = true;
    }
    return frame.Ref();
  });
  pipeline.SetOnEnded([&] { ended = ++events; });
  pipeline.SetOnStop([&] { stopped = ++events; });
  const auto output = pipeline.AddPushUrl(recording.path());
  pipeline.Start(std::string(MW_STREAMER_PIPELINE_TEST_DATA_DIR) +
                 "/h265_cuda.mp4");
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (pipeline.state() != PipelineState::kEnded &&
         pipeline.state() != PipelineState::kFailed &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  pipeline.Stop();
  INFO(pipeline.status().error_message);
  REQUIRE(pipeline.state() == PipelineState::kEnded);
  CHECK(ready == 1);
  CHECK_FALSE(wrong_device.load());
  CHECK_FALSE(invalid_surface.load());
  CHECK(ended == 1);
  CHECK(stopped == 2);
  REQUIRE(video > 0);
  CHECK(DecodeCudaRecording(output) == video.load());
}

}  // namespace

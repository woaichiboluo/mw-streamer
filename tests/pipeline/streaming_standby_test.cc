#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/hardware_context.h"
#include "mw/pipeline/internal/streaming/frame_synchronizer.h"
#include "mw/pipeline/internal/streaming/standby_video_frame.h"

namespace {

using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::HardwareContext;
using mw::streamer::pipeline::internal::streaming::FrameSynchronizer;
using mw::streamer::pipeline::internal::streaming::StandbyVideoFrame;

Frame MakeVideoFrame(std::int64_t pts, std::uint8_t marker = 1) {
  Frame frame;
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = 64;
  frame->height = 36;
  frame->time_base = {1, 90000};
  frame->pts = pts;
  mw::streamer::ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 0),
                                     "分配测试视频帧");
  frame->data[0][0] = marker;
  return frame;
}

Frame MakeAudioFrame(std::int64_t pts, float marker = 1.0F) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 960;
  frame->time_base = {1, 48000};
  frame->pts = pts;
  av_channel_layout_default(&frame->ch_layout, 2);
  mw::streamer::ffmpeg::ThrowIfError(av_frame_get_buffer(frame.get(), 0),
                                     "分配测试音频帧");
  *reinterpret_cast<float*>(frame->extended_data[0]) = marker;
  return frame;
}

std::uint8_t VideoMarker(const Frame& frame) { return frame->data[0][0]; }

float AudioMarker(const Frame& frame) {
  return *reinterpret_cast<const float*>(frame->extended_data[0]);
}

class BufferRef final {
 public:
  explicit BufferRef(AVBufferRef* value) : value_(value) {}
  ~BufferRef() { av_buffer_unref(&value_); }

  BufferRef(const BufferRef&) = delete;
  BufferRef& operator=(const BufferRef&) = delete;

  AVBufferRef* get() const noexcept { return value_; }

 private:
  AVBufferRef* value_;
};

std::filesystem::path WriteTestImage() {
  const auto path =
      std::filesystem::temp_directory_path() / "mw-standby-image.ppm";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n2 1\n255\n";
  const char pixels[] = {static_cast<char>(255), 0, 0, 0,
                         static_cast<char>(255), 0};
  output.write(pixels, sizeof(pixels));
  return path;
}

}  // namespace

TEST_CASE("FrameSynchronizer从零开始输出并连续衔接视频备播") {
  FrameSynchronizer synchronizer(
      false, true, {25, 1}, std::chrono::milliseconds(200), true, "", nullptr);
  auto first = MakeVideoFrame(9000);
  synchronizer.PushVideo(std::move(first));
  auto output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_VIDEO);
  CHECK(output->frame->pts == 0);
  CHECK(output->frame->duration == 3600);
  CHECK(output->force_key_frame);

  synchronizer.Interrupt(AVMEDIA_TYPE_VIDEO);
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->frame->pts == 3600);
  CHECK(output->force_key_frame);

  auto recovered = MakeVideoFrame(450000);
  synchronizer.PushVideo(std::move(recovered));
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->frame->pts == 7200);
  CHECK(output->force_key_frame);

  auto following = MakeVideoFrame(453600);
  synchronizer.PushVideo(std::move(following));
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->frame->pts == 10800);
}

TEST_CASE("FrameSynchronizer使用共同起点交错输出音视频") {
  FrameSynchronizer synchronizer(
      true, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.PushAudio(MakeAudioFrame(48000));
  synchronizer.PushVideo(MakeVideoFrame(90000));

  auto output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_VIDEO);
  CHECK(output->frame->pts == 0);
  CHECK(output->force_key_frame);

  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_AUDIO);
  CHECK(output->frame->pts == 0);
  CHECK(output->frame->duration == 960);

  synchronizer.PushAudio(MakeAudioFrame(48960));
  synchronizer.PushVideo(MakeVideoFrame(93600));
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_AUDIO);
  CHECK(output->frame->pts == 960);
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_VIDEO);
  CHECK(output->frame->pts == 3600);
}

TEST_CASE("FrameSynchronizer对称填补音视频时间洞") {
  FrameSynchronizer synchronizer(
      true, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.PushAudio(MakeAudioFrame(0));
  synchronizer.PushVideo(MakeVideoFrame(0));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));

  synchronizer.PushAudio(MakeAudioFrame(4800, 2.0F));
  synchronizer.PushVideo(MakeVideoFrame(9000, 2));

  std::int64_t real_audio_time_us = AV_NOPTS_VALUE;
  std::int64_t real_video_time_us = AV_NOPTS_VALUE;
  for (int index = 0; index < 16; ++index) {
    auto output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
    if (!output) {
      break;
    }
    if (output->media_type == AVMEDIA_TYPE_AUDIO &&
        AudioMarker(output->frame) == 2.0F) {
      real_audio_time_us = av_rescale_q(
          output->frame->pts, output->frame->time_base, AVRational{1, 1000000});
    }
    if (output->media_type == AVMEDIA_TYPE_VIDEO &&
        VideoMarker(output->frame) == 2) {
      real_video_time_us = av_rescale_q(
          output->frame->pts, output->frame->time_base, AVRational{1, 1000000});
    }
  }

  REQUIRE(real_audio_time_us != AV_NOPTS_VALUE);
  REQUIRE(real_video_time_us != AV_NOPTS_VALUE);
  CHECK(std::abs(real_audio_time_us - real_video_time_us) <= 40000);
}

TEST_CASE("FrameSynchronizer对称处理冻结的音视频时间戳") {
  FrameSynchronizer synchronizer(
      true, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.PushAudio(MakeAudioFrame(0));
  synchronizer.PushVideo(MakeVideoFrame(0));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));

  synchronizer.PushAudio(MakeAudioFrame(0, 2.0F));
  synchronizer.PushVideo(MakeVideoFrame(0, 2));
  auto output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_AUDIO);
  CHECK(AudioMarker(output->frame) == 0.0F);
  CHECK_FALSE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));

  synchronizer.PushAudio(MakeAudioFrame(0, 3.0F));
  synchronizer.PushVideo(MakeVideoFrame(0, 3));
  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_VIDEO);
  CHECK(VideoMarker(output->frame) == 1);
}

TEST_CASE("FrameSynchronizer在队列清空后仍拒绝PTS回退") {
  FrameSynchronizer synchronizer(
      true, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.PushAudio(MakeAudioFrame(0));
  synchronizer.PushVideo(MakeVideoFrame(0));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));

  CHECK_THROWS_AS(synchronizer.PushAudio(MakeAudioFrame(-960)),
                  std::invalid_argument);
  CHECK_THROWS_AS(synchronizer.PushVideo(MakeVideoFrame(-3600)),
                  std::invalid_argument);
}

TEST_CASE("FrameSynchronizer在轨道结束后立即补齐尾帧") {
  FrameSynchronizer synchronizer(
      true, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.PushAudio(MakeAudioFrame(0));
  synchronizer.PushVideo(MakeVideoFrame(0));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));
  REQUIRE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));

  synchronizer.PushAudio(MakeAudioFrame(960));
  synchronizer.PushAudio(MakeAudioFrame(1920));
  synchronizer.Finish(AVMEDIA_TYPE_VIDEO);
  synchronizer.Finish(AVMEDIA_TYPE_AUDIO);

  auto output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_AUDIO);
  CHECK(output->frame->pts == 960);

  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_VIDEO);
  CHECK(output->frame->pts == 3600);

  output = synchronizer.TakeReady(FrameSynchronizer::Clock::now());
  REQUIRE(output);
  CHECK(output->media_type == AVMEDIA_TYPE_AUDIO);
  CHECK(output->frame->pts == 1920);
  CHECK_FALSE(synchronizer.TakeReady(FrameSynchronizer::Clock::now()));
  CHECK(synchronizer.finished());
}

TEST_CASE("FrameSynchronizer拒绝没有产生帧的轨道") {
  FrameSynchronizer synchronizer(
      false, true, {25, 1}, std::chrono::milliseconds(200), false, "", nullptr);
  synchronizer.Finish(AVMEDIA_TYPE_VIDEO);

  CHECK_THROWS_AS(synchronizer.TakeReady(FrameSynchronizer::Clock::now()),
                  std::runtime_error);
}

TEST_CASE("built-in standby image is prepared once in encoder format") {
  auto prototype = MakeVideoFrame(0);
  StandbyVideoFrame standby("");

  standby.Prepare(prototype, nullptr);
  REQUIRE(standby.prepared());
  auto frame = standby.Ref();
  CHECK(frame->format == AV_PIX_FMT_RGBA);
  CHECK(frame->width == 64);
  CHECK(frame->height == 36);
  CHECK(frame->pts == AV_NOPTS_VALUE);
  CHECK(frame->time_base.num == 1);
  CHECK(frame->time_base.den == 90000);

  const auto* background = frame->data[0];
  bool contains_foreground = false;
  for (int row = 0; row < frame->height && !contains_foreground; ++row) {
    const auto* line = frame->data[0] + row * frame->linesize[0];
    for (int column = 0; column < frame->width; ++column) {
      const auto* pixel = line + column * 4;
      if (pixel[0] != background[0] || pixel[1] != background[1] ||
          pixel[2] != background[2]) {
        contains_foreground = true;
        break;
      }
    }
  }
  CHECK(contains_foreground);
}

TEST_CASE("custom standby image is aspect fitted on a dark canvas") {
  const auto path = WriteTestImage();
  auto prototype = MakeVideoFrame(0);
  prototype->width = 40;
  prototype->height = 40;
  StandbyVideoFrame standby(path.string());

  standby.Prepare(prototype, nullptr);
  auto frame = standby.Ref();
  const auto* top = frame->data[0];
  const auto* center = frame->data[0] + 20 * frame->linesize[0] + 10 * 4;
  CHECK(top[0] == 20);
  CHECK(top[1] == 25);
  CHECK(top[2] == 34);
  CHECK((center[0] != 20 || center[1] != 25 || center[2] != 34));

  std::filesystem::remove(path);
}

TEST_CASE("CUDA standby image reuses the encoder prototype frame pool") {
  const auto hardware_context = HardwareContext::CreateCuda(0);
  BufferRef device(av_buffer_ref(hardware_context.get()));
  REQUIRE(device.get());
  BufferRef frames(av_hwframe_ctx_alloc(device.get()));
  REQUIRE(frames.get());
  auto* context = reinterpret_cast<AVHWFramesContext*>(frames.get()->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = AV_PIX_FMT_NV12;
  context->width = 64;
  context->height = 64;
  mw::streamer::ffmpeg::ThrowIfError(av_hwframe_ctx_init(frames.get()),
                                     "初始化测试CUDA帧池");

  Frame prototype;
  mw::streamer::ffmpeg::ThrowIfError(
      av_hwframe_get_buffer(frames.get(), prototype.get(), 0),
      "分配测试CUDA原型帧");
  prototype->time_base = {1, 90000};
  prototype->pts = 0;

  StandbyVideoFrame standby("");
  standby.Prepare(prototype, &hardware_context);
  auto frame = standby.Ref();
  REQUIRE(frame->hw_frames_ctx);
  CHECK(frame->format == AV_PIX_FMT_CUDA);
  CHECK(frame->hw_frames_ctx->data == prototype->hw_frames_ctx->data);
  CHECK(frame->time_base.num == 1);
  CHECK(frame->time_base.den == 90000);
}

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/media_frame.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::AvEncoder;
using mw::streamer::AvEncoderConfig;
using mw::streamer::MediaFrame;
using namespace std::chrono_literals;

AvEncoderConfig Config() {
  AvEncoderConfig config;
  config.width = 64;
  config.height = 64;
  config.video_options = {{"preset", "ultrafast"}, {"tune", "zerolatency"}};
  return config;
}

MediaFrame VideoFrame(int width = 64, int height = 64) {
  MediaFrame media;
  media.video.emplace();
  auto& frame = *media.video;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = height;
  frame->pts = 12345;
  frame->time_base = {1, 90000};
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配视频测试帧失败");
  }
  for (int plane = 0; plane < 2; ++plane) {
    const int rows = plane == 0 ? height : height / 2;
    for (int row = 0; row < rows; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane],
                  plane == 0 ? 80 : 128, static_cast<std::size_t>(width));
    }
  }
  return media;
}

MediaFrame AudioFrame(int samples) {
  MediaFrame media;
  media.audio.emplace();
  auto& frame = *media.audio;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->pts = 67890;
  frame->time_base = {1, 90000};
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配音频测试帧失败");
  }
  for (int channel = 0; channel < 2; ++channel) {
    std::memset(frame->extended_data[channel], 0,
                static_cast<std::size_t>(samples) * sizeof(float));
  }
  return media;
}

struct Capture {
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<ffmpeg::Packet> packets;
  std::exception_ptr error;
  int error_count = 0;

  void Bind(AvEncoder& encoder) {
    encoder.SetOnPacket([&](const ffmpeg::Packet& packet) {
      std::lock_guard lock(mutex);
      packets.push_back(packet.Ref());
      wake.notify_all();
    });
    encoder.SetOnError([&](std::exception_ptr exception) {
      std::lock_guard lock(mutex);
      error = exception;
      ++error_count;
      wake.notify_all();
    });
  }

  bool WaitPackets(std::size_t count) {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] {
      return packets.size() >= count || error;
    }) && !error;
  }

  bool WaitError() {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] { return error != nullptr; });
  }
};

struct Gate {
  std::mutex mutex;
  std::condition_variable wake;
  bool entered = false;
  bool released = false;

  void Enter() {
    std::unique_lock lock(mutex);
    entered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return released; });
  }

  bool Wait() {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] { return entered; });
  }

  void Release() {
    std::lock_guard lock(mutex);
    released = true;
    wake.notify_all();
  }
};

struct ReleaseGate {
  Gate& gate;
  ~ReleaseGate() { gate.Release(); }
};

struct FreeCodec {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

// Decode with native FFmpeg rather than the retired high-level decoder wrapper.
std::vector<ffmpeg::Frame> Decode(const ffmpeg::StreamInfo& stream,
                                  const std::vector<ffmpeg::Packet>& packets) {
  const AVCodec* codec =
      avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
  if (!codec) {
    throw std::runtime_error("测试解码器不存在");
  }
  std::unique_ptr<AVCodecContext, FreeCodec> context(
      avcodec_alloc_context3(codec));
  if (!context ||
      avcodec_parameters_to_context(context.get(),
                                    stream.codec_parameters.get()) < 0 ||
      avcodec_open2(context.get(), codec, nullptr) < 0) {
    throw std::runtime_error("打开测试解码器失败");
  }
  std::vector<ffmpeg::Frame> frames;
  auto receive = [&] {
    while (true) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(context.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return;
      }
      if (result < 0) {
        throw std::runtime_error("解码测试 packet 失败");
      }
      frames.push_back(std::move(frame));
    }
  };
  for (const auto& packet : packets) {
    if (packet->stream_index != stream.stream_index) {
      continue;
    }
    if (avcodec_send_packet(context.get(), packet.get()) < 0) {
      throw std::runtime_error("提交测试 packet 失败");
    }
    receive();
  }
  if (avcodec_send_packet(context.get(), nullptr) < 0) {
    throw std::runtime_error("排空测试解码器失败");
  }
  receive();
  return frames;
}

struct FreeBuffer {
  void operator()(AVBufferRef* reference) const { av_buffer_unref(&reference); }
};

std::unique_ptr<ffmpeg::HwDeviceContext> CudaDevice() {
  try {
    return std::make_unique<ffmpeg::HwDeviceContext>(
        ffmpeg::HwDeviceType::kCuda);
  } catch (const std::exception&) {
    return nullptr;
  }
}

ffmpeg::Frame GpuFrame(ffmpeg::HwDeviceContext& device,
                       AVPixelFormat software_format, int width = 64,
                       int height = 64) {
  std::unique_ptr<AVBufferRef, FreeBuffer> pool(
      av_hwframe_ctx_alloc(device.get()));
  if (!pool) {
    throw std::bad_alloc();
  }
  auto* context = reinterpret_cast<AVHWFramesContext*>(pool->data);
  context->format = AV_PIX_FMT_CUDA;
  context->sw_format = software_format;
  context->width = width;
  context->height = height;
  context->initial_pool_size = 2;
  if (av_hwframe_ctx_init(pool.get()) < 0) {
    throw std::runtime_error("初始化测试 GPU 帧池失败");
  }
  ffmpeg::Frame result;
  if (av_hwframe_get_buffer(pool.get(), result.get(), 0) < 0) {
    throw std::runtime_error("分配测试 GPU 帧失败");
  }
  if (software_format == AV_PIX_FMT_NV12) {
    const auto source = VideoFrame(width, height);
    if (av_hwframe_transfer_data(result.get(), source.video->get(), 0) < 0 ||
        av_frame_copy_props(result.get(), source.video->get()) < 0) {
      throw std::runtime_error("上传测试 GPU 帧失败");
    }
  }
  return result;
}

TEST_CASE("AvEncoder 同步初始化按配置提供单轨或双轨描述",
          "[av-encoder][configuration]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  AvEncoder encoder(cpu);
  auto config = Config();
  static_assert(noexcept(encoder.Start(config)));
  SECTION("默认双轨") {}
  SECTION("纯视频") { config.audio_sample_format = AV_SAMPLE_FMT_NONE; }
  SECTION("纯音频") { config.video_pixel_format = AV_PIX_FMT_NONE; }
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  const bool has_video = config.video_pixel_format != AV_PIX_FMT_NONE;
  const bool has_audio = config.audio_sample_format != AV_SAMPLE_FMT_NONE;
  REQUIRE(streams.size() == static_cast<std::size_t>(has_video + has_audio));
  int videos = 0;
  int audios = 0;
  for (const auto& stream : streams) {
    stream.Validate();
    const auto* parameters = stream.codec_parameters.get();
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      ++videos;
      CHECK(parameters->codec_id == AV_CODEC_ID_H264);
      CHECK(parameters->width == 64);
      CHECK(parameters->height == 64);
      CHECK(stream.time_base.num == 1);
      CHECK(stream.time_base.den == 30);
    } else {
      ++audios;
      CHECK(parameters->codec_type == AVMEDIA_TYPE_AUDIO);
      CHECK(parameters->codec_id == AV_CODEC_ID_AAC);
      CHECK(parameters->sample_rate == 48000);
      CHECK(parameters->ch_layout.nb_channels == 2);
      CHECK(stream.time_base.num == 1);
      CHECK(stream.time_base.den == 48000);
    }
  }
  CHECK(videos == static_cast<int>(has_video));
  CHECK(audios == static_cast<int>(has_audio));
  if (streams.size() == 2) {
    CHECK(streams[0].stream_index != streams[1].stream_index);
  }
  CHECK_FALSE(encoder.Start(config));
  CHECK(encoder.streams().size() == streams.size());
  CHECK_FALSE(encoder.error());
  encoder.Stop();
  REQUIRE(encoder.Start(config));
  encoder.Stop();
}

TEST_CASE("AvEncoder 视频 PTS 由帧序号生成且不修改调用方帧",
          "[av-encoder][timing][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.fps = {30000, 1001};
  REQUIRE(encoder.Start(config));
  auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  // Returned track descriptions own their parameters independently.
  streams[0].codec_parameters.get()->width = 32;
  CHECK(encoder.streams()[0].codec_parameters.get()->width == 64);
  streams = encoder.streams();
  const auto source = VideoFrame();
  for (int index = 0; index < 3; ++index) {
    REQUIRE(encoder.SubmitFrame(source));
    REQUIRE(capture.WaitPackets(static_cast<std::size_t>(index + 1)));
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() == 3);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == streams[0].stream_index);
    CHECK(packet->pts == static_cast<int64_t>(index));
    CHECK(packet->dts == static_cast<int64_t>(index));
    CHECK(packet->time_base.num == 1001);
    CHECK(packet->time_base.den == 30000);
  }
  CHECK(source.video.value()->pts == 12345);
  CHECK(source.video.value()->time_base.den == 90000);
  const auto decoded = Decode(streams[0], capture.packets);
  REQUIRE(decoded.size() == 3);
  CHECK(decoded.back()->width == 64);
  CHECK(decoded.back()->height == 64);
  CHECK_FALSE(encoder.SubmitFrame(source));
}

TEST_CASE("AvEncoder 自行对齐 AAC frame_size 并在 Stop 排空尾部",
          "[av-encoder][audio][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.video_pixel_format = AV_PIX_FMT_NONE;
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  const auto source = AudioFrame(3008);
  REQUIRE(encoder.SubmitFrame(source));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() >= 3);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == streams[0].stream_index);
    CHECK(packet->time_base.num == 1);
    CHECK(packet->time_base.den == 48000);
    if (index != 0) {
      CHECK(packet->pts - capture.packets[index - 1]->pts == 1024);
    }
  }
  int decoded_samples = 0;
  for (const auto& frame : Decode(streams[0], capture.packets)) {
    CHECK(frame->sample_rate == 48000);
    CHECK(frame->ch_layout.nb_channels == 2);
    decoded_samples += frame->nb_samples;
  }
  // Native AAC contributes encoder priming and may pad the final frame.
  CHECK(decoded_samples >= 3008);
  CHECK(decoded_samples < 3008 + 2048);
  CHECK(source.audio.value()->pts == 67890);
  CHECK(source.audio.value()->nb_samples == 3008);
  CHECK(source.audio.value()->time_base.den == 90000);
}

TEST_CASE("AvEncoder 双轨并行产包且 Stop 排空延迟视频编码",
          "[av-encoder][parallel][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  // Leave video packets buffered until Stop, while audio can produce packets.
  config.video_options.erase("tune");
  config.max_b_frames = 2;
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 2);
  auto source = VideoFrame();
  source.audio = AudioFrame(3008).audio;
  REQUIRE(encoder.SubmitFrame(source));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  for (const auto& stream : streams) {
    const auto frames = Decode(stream, capture.packets);
    REQUIRE_FALSE(frames.empty());
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_VIDEO) {
      CHECK(frames.size() == 1);
    } else {
      int samples = 0;
      for (const auto& frame : frames) {
        samples += frame->nb_samples;
      }
      CHECK(samples >= 3008);
    }
  }
}

TEST_CASE("AvEncoder 析构排空编码缓存", "[av-encoder][lifecycle][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  std::vector<ffmpeg::StreamInfo> streams;
  {
    AvEncoder encoder(cpu);
    capture.Bind(encoder);
    auto config = Config();
    config.audio_sample_format = AV_SAMPLE_FMT_NONE;
    config.video_options.erase("tune");
    config.max_b_frames = 2;
    REQUIRE(encoder.Start(config));
    streams = encoder.streams();
    REQUIRE(encoder.SubmitFrame(VideoFrame()));
  }
  REQUIRE(capture.packets.size() == 1);
  CHECK(Decode(streams.front(), capture.packets).size() == 1);
  CHECK(capture.error_count == 0);
}

TEST_CASE("AvEncoder 队列满丢弃新帧但保留视频时间轴间隔",
          "[av-encoder][queue][timing]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate gate;
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet) {
    {
      std::lock_guard lock(capture.mutex);
      capture.packets.push_back(packet.Ref());
      capture.wake.notify_all();
    }
    gate.Enter();
  });
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(config));
  ReleaseGate release_before_encoder{gate};
  const auto source = VideoFrame();
  REQUIRE(encoder.SubmitFrame(source));
  REQUIRE(gate.Wait());
  REQUIRE(encoder.SubmitFrame(source));
  REQUIRE(encoder.SubmitFrame(source));
  gate.Release();
  REQUIRE(capture.WaitPackets(2));
  REQUIRE(encoder.SubmitFrame(source));
  encoder.Stop();
  REQUIRE(capture.packets.size() == 3);
  CHECK(capture.packets[0]->pts == 0);
  CHECK(capture.packets[1]->pts == 1);
  CHECK(capture.packets[2]->pts == 3);
}

TEST_CASE("AvEncoder Stop 等待回调并排空已经接收的帧",
          "[av-encoder][stop][backpressure]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate gate;
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet) {
    {
      std::lock_guard lock(capture.mutex);
      capture.packets.push_back(packet.Ref());
    }
    gate.Enter();
  });
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(config));
  ReleaseGate release_before_encoder{gate};
  REQUIRE(encoder.SubmitFrame(VideoFrame()));
  REQUIRE(gate.Wait());
  REQUIRE(encoder.SubmitFrame(VideoFrame()));
  auto stopping = std::async(std::launch::async, [&] { encoder.Stop(); });
  ReleaseGate release_before_future{gate};
  CHECK(stopping.wait_for(50ms) == std::future_status::timeout);
  gate.Release();
  REQUIRE(stopping.wait_for(10s) == std::future_status::ready);
  stopping.get();
  REQUIRE(capture.packets.size() == 2);
  CHECK_FALSE(encoder.error());
}

TEST_CASE("AvEncoder 同步初始化失败可恢复且不尝试其他编码器",
          "[av-encoder][configuration][hardware]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto invalid = Config();
  SECTION("不存在的编码器") {
    invalid.video_encoder_name = "mw_encoder_does_not_exist";
  }
  SECTION("视频打开后音频初始化失败") {
    invalid.audio_encoder_name = "mw_encoder_does_not_exist";
  }
  SECTION("无启用轨道") {
    invalid.video_pixel_format = AV_PIX_FMT_NONE;
    invalid.audio_sample_format = AV_SAMPLE_FMT_NONE;
  }
  SECTION("无效像素格式") { invalid.video_pixel_format = AV_PIX_FMT_NB; }
  SECTION("私有选项不能覆盖正式码率字段") { invalid.video_options["b"] = "1M"; }
  SECTION("CPU 设备不能满足指定的硬件编码器") {
    invalid.video_encoder_name = "h264_nvenc";
  }
  SECTION("拼错的私有选项") {
    invalid.video_options["preset_typo"] = "ultrafast";
  }
  CHECK_FALSE(encoder.Start(invalid));
  REQUIRE(encoder.error());
  CHECK(encoder.streams().empty());
  CHECK_FALSE(encoder.SubmitFrame(VideoFrame()));
  encoder.Stop();
  CHECK(capture.error_count == 0);
  CHECK(capture.packets.empty());
  auto valid = Config();
  valid.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(valid));
  REQUIRE(encoder.SubmitFrame(VideoFrame()));
  encoder.Stop();
  REQUIRE(capture.packets.size() == 1);
  CHECK_FALSE(encoder.error());
}

TEST_CASE("AvEncoder 编码和 packet 回调异常通过 exception_ptr 上报一次",
          "[av-encoder][error]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  auto frame = VideoFrame();
  SECTION("视频帧格式不匹配") { frame.video.value()->width = 32; }
  SECTION("packet 消费者抛异常") {
    encoder.SetOnPacket([](const ffmpeg::Packet&) {
      throw std::runtime_error("测试 packet 消费异常");
    });
  }
  REQUIRE(encoder.Start(config));
  REQUIRE(encoder.SubmitFrame(frame));
  REQUIRE(capture.WaitError());
  encoder.Stop();
  CHECK(capture.error_count == 1);
  REQUIRE(capture.error);
  REQUIRE(encoder.error());
  CHECK_THROWS(std::rethrow_exception(capture.error));
  CHECK_FALSE(encoder.SubmitFrame(VideoFrame()));
  encoder.Stop();
  CHECK(capture.error_count == 1);
}

TEST_CASE("AvEncoder 共享 CUDA 设备支持 CPU 上传与 GPU 帧直接编码",
          "[av-encoder][gpu]") {
  if (!avcodec_find_encoder_by_name("h264_nvenc")) {
    SKIP("当前 FFmpeg 不提供 h264_nvenc");
  }
  auto device = CudaDevice();
  if (!device) {
    SKIP("当前环境不能创建 CUDA 设备");
  }
  Capture capture;
  AvEncoder encoder(*device);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.width = 256;
  config.height = 256;
  config.video_encoder_name = "h264_nvenc";
  config.video_options = {{"delay", "0"}};
  // A usable device and an installed codec make subsequent errors test
  // failures.
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  CHECK(streams[0].codec_parameters.get()->codec_id == AV_CODEC_ID_H264);
  const auto cpu = VideoFrame(config.width, config.height);
  REQUIRE(encoder.SubmitFrame(cpu));
  REQUIRE(capture.WaitPackets(1));
  MediaFrame gpu;
  gpu.video = GpuFrame(*device, AV_PIX_FMT_NV12, config.width, config.height);
  const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
      gpu.video.value()->hw_frames_ctx->data);
  REQUIRE(pool->device_ref->data == device->get()->data);
  REQUIRE(encoder.SubmitFrame(gpu));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() == 2);
  CHECK(capture.packets[0]->pts == 0);
  CHECK(capture.packets[1]->pts == 1);
  const auto frames = Decode(streams[0], capture.packets);
  REQUIRE(frames.size() == 2);
  for (const auto& frame : frames) {
    CHECK(frame->width == config.width);
    CHECK(frame->height == config.height);
    CHECK(frame->data[0][0] >= 76);
    CHECK(frame->data[0][0] <= 84);
  }
  CHECK(cpu.video.value()->pts == 12345);
  CHECK(gpu.video.value()->pts == 12345);
}

TEST_CASE("AvEncoder 硬件帧的软件布局不匹配通过 OnError 上报",
          "[av-encoder][gpu][error]") {
  if (!avcodec_find_encoder_by_name("h264_nvenc")) {
    SKIP("当前 FFmpeg 不提供 h264_nvenc");
  }
  auto device = CudaDevice();
  if (!device) {
    SKIP("当前环境不能创建 CUDA 设备");
  }
  Capture capture;
  AvEncoder encoder(*device);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.width = 256;
  config.height = 256;
  config.video_encoder_name = "h264_nvenc";
  config.video_options.clear();
  REQUIRE(encoder.Start(config));
  MediaFrame incompatible;
  incompatible.video =
      GpuFrame(*device, AV_PIX_FMT_YUV420P, config.width, config.height);
  REQUIRE(encoder.SubmitFrame(incompatible));
  REQUIRE(capture.WaitError());
  encoder.Stop();
  REQUIRE(encoder.error());
  CHECK(capture.error_count == 1);
  CHECK(capture.packets.empty());
}

TEST_CASE("AvEncoder 外部 GPU 设备不改变显式软件编码器选择",
          "[av-encoder][gpu][configuration]") {
  auto device = CudaDevice();
  if (!device) {
    SKIP("当前环境不能创建 CUDA 设备");
  }
  Capture capture;
  AvEncoder encoder(*device);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  CHECK(streams[0].codec_parameters.get()->format == AV_PIX_FMT_NV12);
  REQUIRE(encoder.SubmitFrame(VideoFrame()));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK(Decode(streams[0], capture.packets).size() == 1);
}

}  // namespace

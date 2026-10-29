#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/encoder/av_encoder.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/platform/wait_signal.h"
#include "mw/streamer/platform/write_signal.h"
#include "mw/streamer/syncer/syncer.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::AvEncoder;
using mw::streamer::AvEncoderConfig;
using mw::streamer::internal::WaitSignal;
using mw::streamer::internal::WriteSignal;
using namespace std::chrono_literals;

AvEncoderConfig Config() {
  AvEncoderConfig config;
  config.width = 64;
  config.height = 64;
  config.video_options = {{"preset", "ultrafast"}, {"tune", "zerolatency"}};
  return config;
}

ffmpeg::Frame VideoFrame(int width = 64, int height = 64) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = height;
  frame->pts = 90000;
  frame->duration = 3000;
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
  return frame;
}

ffmpeg::Frame AudioFrame(int samples) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->pts = 48000;
  frame->time_base = {1, 48000};
  frame->duration = samples;
  av_channel_layout_default(&frame->ch_layout, 2);
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    throw std::runtime_error("分配音频测试帧失败");
  }
  for (int channel = 0; channel < 2; ++channel) {
    std::memset(frame->extended_data[channel], 0,
                static_cast<std::size_t>(samples) * sizeof(float));
  }
  return frame;
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

  bool WaitStreamPackets(int stream_index, std::size_t count) {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] {
      return error ||
             std::count_if(packets.begin(), packets.end(),
                           [&](const auto& packet) {
                             return packet->stream_index == stream_index;
                           }) >= static_cast<std::ptrdiff_t>(count);
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
  int entrances = 0;

  void Enter() {
    std::unique_lock lock(mutex);
    entered = true;
    ++entrances;
    wake.notify_all();
    wake.wait(lock, [&] { return released; });
  }

  bool Wait() {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] { return entered; });
  }

  bool WaitBoth() {
    std::unique_lock lock(mutex);
    return wake.wait_for(lock, 10s, [&] { return entrances >= 2; });
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

// These timing/drain tests choose to retain and retry rejected inputs. Queue
// rejection itself is tested separately with blocked callbacks and no retry.
template <typename Push>
bool RetryUntilAccepted(Push push) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (push()) return true;
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

class WarningLog final {
 public:
  WarningLog()
      : path_(
            std::filesystem::temp_directory_path() /
            ("mw-encoder-queue-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             ".log")) {
    mw::log::LogConfig config;
    config.console.enabled = false;
    config.rotating_file.enabled = true;
    config.rotating_file.path = path_.string();
    config.modules = {{"streamer", mw::log::LogLevel::kWarning}};
    logging_ = std::make_unique<mw::log::Logging>(config);
  }
  ~WarningLog() {
    logging_.reset();
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string Read() {
    logging_.reset();
    std::ifstream file(path_);
    if (!file.is_open()) throw std::runtime_error("读取编码器告警日志失败");
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
  }

 private:
  std::filesystem::path path_;
  std::unique_ptr<mw::log::Logging> logging_;
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
  context->pkt_timebase = stream.time_base;
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
    if (av_hwframe_transfer_data(result.get(), source.get(), 0) < 0 ||
        av_frame_copy_props(result.get(), source.get()) < 0) {
      throw std::runtime_error("上传测试 GPU 帧失败");
    }
  }
  result->pts = 90000;
  result->time_base = {1, 90000};
  result->duration = 3000;
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

TEST_CASE("AvEncoder 保留视频非零起点与缺口并换算分数帧率时间基",
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
  auto source = VideoFrame();
  const int64_t expected_pts[]{10, 11, 14};
  for (int index = 0; index < 3; ++index) {
    source->pts = expected_pts[index] * 3003;
    source->duration = 3003;
    REQUIRE(encoder.TryPushVideo(source));
    REQUIRE(capture.WaitPackets(static_cast<std::size_t>(index + 1)));
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() == 3);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == streams[0].stream_index);
    CHECK(packet->pts == expected_pts[index]);
    CHECK(packet->dts == expected_pts[index]);
    CHECK(packet->duration == 1);
    CHECK(packet->time_base.num == 1001);
    CHECK(packet->time_base.den == 30000);
  }
  CHECK(source->pts == 42042);
  CHECK(source->duration == 3003);
  CHECK(source->time_base.den == 90000);
  const auto decoded = Decode(streams[0], capture.packets);
  REQUIRE(decoded.size() == 3);
  CHECK(decoded.back()->width == 64);
  CHECK(decoded.back()->height == 64);
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    CHECK(decoded[index]->pts == expected_pts[index]);
  }
  CHECK_FALSE(encoder.TryPushVideo(source));
}

TEST_CASE("AvEncoder 自行对齐 AAC frame_size 并在 Stop 排空尾部",
          "[av-encoder][audio][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.video_pixel_format = AV_PIX_FMT_NONE;
  SECTION("默认帧率") {}
  SECTION("未指定帧率") { config.fps = {0, 0}; }
  SECTION("零分子") { config.fps = {0, 1}; }
  SECTION("零分母") { config.fps = {30, 0}; }
  SECTION("负帧率") { config.fps = {-30, 1}; }
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  auto source = AudioFrame(3008);
  source->pts = 1500;
  source->time_base = {1, 1000};
  source->duration = 0;
  REQUIRE(encoder.TryPushAudio(source));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() == 4);
  const int priming = streams[0].codec_parameters.get()->initial_padding;
  REQUIRE(priming == 1024);
  CHECK(capture.packets.front()->pts == 72000 - priming);
  CHECK(capture.packets.back()->pts == 72000 + 2048);
  // Native FFmpeg emits a short 960-sample tail; AAC retains its duration.
  CHECK(capture.packets.back()->duration == 960);
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
  CHECK(source->pts == 1500);
  CHECK(source->duration == 0);
  CHECK(source->nb_samples == 3008);
  CHECK(source->time_base.den == 1000);
}

TEST_CASE("Syncer 无音频输入生成的静音经 AAC 编解码后保持静音",
          "[av-encoder][audio][silence][drain]") {
  const auto format =
      GENERATE(AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_U8);
  auto config = Config();
  config.video_pixel_format = AV_PIX_FMT_NONE;
  config.audio_sample_format = format;
  mw::streamer::SyncerConfig syncer_config;
  syncer_config.encoder = config;
  syncer_config.options.initial_timeout = 20ms;
  mw::streamer::Syncer syncer(syncer_config);
  std::vector<ffmpeg::Frame> silence;
  constexpr std::size_t kFrameCount = 160;
  std::int64_t now_us = 0;
  std::int64_t source_samples = 0;
  bool has_short_block = false;
  // Advancing the simulated clock also exercises the short fillers at the
  // edge of a timeout batch, without sleeping through the shared clock hold.
  for (int step = 0; step < 4096 && silence.size() < kFrameCount; ++step) {
    auto result = syncer.Step(false, nullptr, now_us);
    if (result.frame) {
      const auto& frame = *result.frame;
      REQUIRE(frame->format == format);
      REQUIRE(frame->sample_rate == 48000);
      REQUIRE(frame->ch_layout.nb_channels == 2);
      REQUIRE(frame->nb_samples > 0);
      REQUIRE(frame->nb_samples <= 1024);
      CHECK(frame->pts == source_samples);
      CHECK(frame->duration == frame->nb_samples);
      CHECK(frame->time_base.num == 1);
      CHECK(frame->time_base.den == 48000);
      const bool planar = av_sample_fmt_is_planar(format) != 0;
      const int planes = planar ? frame->ch_layout.nb_channels : 1;
      const auto plane_bytes =
          static_cast<std::size_t>(frame->nb_samples) *
          static_cast<std::size_t>(av_get_bytes_per_sample(format)) *
          (planar ? 1U : 2U);
      const std::uint8_t expected = format == AV_SAMPLE_FMT_U8 ? 128 : 0;
      for (int plane = 0; plane < planes; ++plane) {
        REQUIRE(frame->extended_data[plane] != nullptr);
        CHECK(std::all_of(
            frame->extended_data[plane],
            frame->extended_data[plane] + plane_bytes,
            [expected](std::uint8_t value) { return value == expected; }));
      }
      source_samples += frame->nb_samples;
      has_short_block = has_short_block || frame->nb_samples < 1024;
      silence.push_back(std::move(*result.frame));
    }
    const auto wake_us = std::max(now_us, result.wake_us);
    now_us = !result.frame && wake_us == now_us ? now_us + 1 : wake_us;
  }
  REQUIRE(silence.size() == kFrameCount);
  REQUIRE(has_short_block);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  REQUIRE(streams[0].codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  for (const auto& frame : silence) {
    REQUIRE(RetryUntilAccepted([&] { return encoder.TryPushAudio(frame); }));
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK_FALSE(capture.error);
  REQUIRE_FALSE(capture.packets.empty());
  const auto decoded = Decode(streams[0], capture.packets);
  REQUIRE_FALSE(decoded.empty());
  bool finite = true;
  double maximum = 0;
  long double squared = 0;
  std::size_t decoded_samples = 0;
  for (const auto& frame : decoded) {
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    REQUIRE(frame->sample_rate == 48000);
    REQUIRE(frame->ch_layout.nb_channels == 2);
    REQUIRE(frame->nb_samples > 0);
    for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
      REQUIRE(frame->extended_data[channel] != nullptr);
      const auto* samples =
          reinterpret_cast<const float*>(frame->extended_data[channel]);
      for (int sample = 0; sample < frame->nb_samples; ++sample) {
        const double value = samples[sample];
        ++decoded_samples;
        if (!std::isfinite(value)) {
          finite = false;
          continue;
        }
        maximum = std::max(maximum, std::abs(value));
        const auto wide_value = static_cast<long double>(value);
        squared += wide_value * wide_value;
      }
    }
  }
  REQUIRE(decoded_samples >= static_cast<std::size_t>(source_samples) * 2);
  const auto rms =
      std::sqrt(squared / static_cast<long double>(decoded_samples));
  CAPTURE(av_get_sample_fmt_name(format), source_samples, decoded_samples,
          maximum, rms);
  CHECK(finite);
  CHECK(maximum <= 1e-6);
  CHECK(rms <= 1e-6L);
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
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
  REQUIRE(encoder.TryPushAudio(AudioFrame(3008)));
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

TEST_CASE("AvEncoder 多个音频小块重新分块后保留双轨相对起点",
          "[av-encoder][audio][timing][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  REQUIRE(encoder.Start(Config()));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 2);
  const int chunks[]{256, 257, 495, 1024, 976};
  int submitted_samples = 0;
  for (std::size_t index = 0; index < std::size(chunks); ++index) {
    auto video = VideoFrame();
    video->pts = 180000 + 3000 * static_cast<std::int64_t>(index);
    auto audio = AudioFrame(chunks[index]);
    audio->pts = 108000 + submitted_samples;
    REQUIRE(encoder.TryPushVideo(video));
    REQUIRE(RetryUntilAccepted([&] { return encoder.TryPushAudio(audio); }));
    REQUIRE(capture.WaitStreamPackets(0, index + 1));
    CHECK(audio->pts == 108000 + submitted_samples);
    CHECK(audio->duration == chunks[index]);
    submitted_samples += chunks[index];
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(submitted_samples == 3008);
  std::vector<ffmpeg::Packet> video;
  std::vector<ffmpeg::Packet> audio;
  for (const auto& packet : capture.packets) {
    (packet->stream_index == 0 ? video : audio).push_back(packet.Ref());
  }
  REQUIRE(video.size() == std::size(chunks));
  REQUIRE(audio.size() == 4);
  CHECK(video.front()->pts == 60);
  CHECK(audio.front()->pts == 108000 - 1024);
  CHECK(audio.back()->pts == 108000 + 2048);
  CHECK(audio.back()->duration == 960);
  // Remove only AAC's native encoder delay when comparing the source origins:
  // video starts at 2 s, audio at 2.25 s, so the 250 ms separation must
  // survive.
  CHECK((audio.front()->pts + 1024) * 30 - video.front()->pts * 48000 ==
        12000 * 30);
  const auto decoded_audio = Decode(streams[1], audio);
  // The native AAC decoder discards its 1024-sample priming packet.
  REQUIRE(decoded_audio.size() == 3);
  CHECK(decoded_audio.front()->pts == 108000);
  int decoded_samples = 0;
  for (const auto& frame : decoded_audio) {
    decoded_samples += frame->nb_samples;
  }
  // AAC decoding pads the final 960 samples to its 1024-sample output block.
  CHECK(decoded_samples == 3072);
  CHECK(Decode(streams[0], video).size() == std::size(chunks));
}

TEST_CASE("AvEncoder 保留原生 B 帧重排的 PTS 和 DTS",
          "[av-encoder][video][timing][drain]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.video_options.erase("tune");
  config.max_b_frames = 2;
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  constexpr int kFrames = 5;
  for (int index = 0; index < kFrames; ++index) {
    auto video = VideoFrame();
    video->pts = 90000 + 3000 * index;
    REQUIRE(RetryUntilAccepted([&] { return encoder.TryPushVideo(video); }));
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  std::vector<ffmpeg::Packet> video;
  for (const auto& packet : capture.packets) {
    if (packet->stream_index == 0) video.push_back(packet.Ref());
  }
  REQUIRE(video.size() == kFrames);
  bool reordered = false;
  for (std::size_t index = 0; index < video.size(); ++index) {
    CHECK(video[index]->pts >= 30);
    CHECK(video[index]->pts < 30 + kFrames);
    CHECK(video[index]->dts != AV_NOPTS_VALUE);
    reordered |= video[index]->pts != video[index]->dts;
    if (index > 0) CHECK(video[index]->dts > video[index - 1]->dts);
  }
  CHECK(reordered);
  const auto decoded = Decode(streams.front(), video);
  REQUIRE(decoded.size() == kFrames);
  for (int index = 0; index < kFrames; ++index) {
    CHECK(decoded[static_cast<std::size_t>(index)]->pts == 30 + index);
  }
}

TEST_CASE("AvEncoder 分轨同步拒绝非法时间描述且保持会话可用",
          "[av-encoder][timing][validation]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto video = VideoFrame();
  auto audio = AudioFrame(3008);
  bool invalid_audio = false;
  SECTION("视频 PTS 缺失") { video->pts = AV_NOPTS_VALUE; }
  SECTION("音频 PTS 缺失") {
    audio->pts = AV_NOPTS_VALUE;
    invalid_audio = true;
  }
  SECTION("视频 time_base 分子为零") { video->time_base = {0, 90000}; }
  SECTION("视频 time_base 分母为负数") { video->time_base = {1, -90000}; }
  SECTION("音频 time_base 分子为负数") {
    audio->time_base = {-1, 48000};
    invalid_audio = true;
  }
  SECTION("音频 time_base 分母为零") {
    audio->time_base = {1, 0};
    invalid_audio = true;
  }
  SECTION("视频 duration 为负数") { video->duration = -1; }
  SECTION("音频 duration 为负数") {
    audio->duration = -1;
    invalid_audio = true;
  }
  REQUIRE(encoder.Start(Config()));
  if (invalid_audio) {
    CHECK_THROWS_AS(encoder.TryPushAudio(audio), std::invalid_argument);
  } else {
    CHECK_THROWS_AS(encoder.TryPushVideo(video), std::invalid_argument);
  }
  CHECK_FALSE(encoder.error());
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
  REQUIRE(encoder.TryPushAudio(AudioFrame(3008)));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK(capture.error_count == 0);
  REQUIRE_FALSE(capture.packets.empty());
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
    REQUIRE(encoder.TryPushVideo(VideoFrame()));
  }
  REQUIRE(capture.packets.size() == 1);
  CHECK(Decode(streams.front(), capture.packets).size() == 1);
  CHECK(capture.error_count == 0);
}

TEST_CASE("AvEncoder 视频队列满拒绝新帧并保留调用方时间轴间隔",
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
  auto source = VideoFrame();
  source->pts = 90000;
  REQUIRE(encoder.TryPushVideo(source));
  REQUIRE(gate.Wait());
  for (int index = 1; index <= 3; ++index) {
    source->pts = 90000 + 3000 * index;
    REQUIRE(encoder.TryPushVideo(source));
  }
  source->pts = 102000;
  REQUIRE_FALSE(encoder.TryPushVideo(source));
  CHECK(source->pts == 102000);
  gate.Release();
  REQUIRE(capture.WaitPackets(4));
  source->pts = 105000;
  REQUIRE(encoder.TryPushVideo(source));
  encoder.Stop();
  REQUIRE(capture.packets.size() == 5);
  const std::array<std::int64_t, 5> expected_pts{30, 31, 32, 33, 35};
  const auto decoded = Decode(encoder.streams().front(), capture.packets);
  REQUIRE(decoded.size() == expected_pts.size());
  for (std::size_t index = 0; index < expected_pts.size(); ++index) {
    CHECK(capture.packets[index]->pts == expected_pts[index]);
    CHECK(decoded[index]->pts == expected_pts[index]);
  }
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
  auto source = VideoFrame();
  REQUIRE(encoder.TryPushVideo(source));
  REQUIRE(gate.Wait());
  for (int index = 0; index < 3; ++index) {
    source->pts += 3000;
    REQUIRE(encoder.TryPushVideo(source));
  }
  source->pts += 3000;
  CHECK_FALSE(encoder.TryPushVideo(source));
  auto stopping = std::async(std::launch::async, [&] { encoder.Stop(); });
  ReleaseGate release_before_future{gate};
  CHECK(stopping.wait_for(50ms) == std::future_status::timeout);
  gate.Release();
  REQUIRE(stopping.wait_for(10s) == std::future_status::ready);
  stopping.get();
  REQUIRE(capture.packets.size() == 4);
  CHECK(Decode(encoder.streams().front(), capture.packets).size() == 4);
  CHECK_FALSE(encoder.error());
}

TEST_CASE("AvEncoder 每轨队列独立拒绝满队列且另一轨继续编码",
          "[av-encoder][queue][parallel][thread]") {
  bool block_audio = false;
  SECTION("视频回调阻塞不影响音频") {}
  SECTION("音频回调阻塞不影响视频") { block_audio = true; }
  WarningLog log;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate gate;
  Capture capture;
  std::array<std::vector<std::thread::id>, 2> callback_threads;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  const int blocked = block_audio ? 1 : 0;
  const int other = 1 - blocked;
  auto wake = std::make_shared<WaitSignal>();
  auto blocked_signal = std::make_shared<WriteSignal>(wake);
  auto other_signal = std::make_shared<WriteSignal>(wake);
  encoder.SetWriteSignal(!block_audio, blocked_signal);
  encoder.SetWriteSignal(block_audio, other_signal);
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet) {
    {
      std::lock_guard lock(capture.mutex);
      capture.packets.push_back(packet.Ref());
      callback_threads[static_cast<std::size_t>(packet->stream_index)]
          .push_back(std::this_thread::get_id());
      capture.wake.notify_all();
    }
    if (packet->stream_index == blocked) gate.Enter();
  });
  REQUIRE(encoder.Start(Config()));
  ReleaseGate release_before_encoder{gate};
  auto pending = block_audio ? AudioFrame(3008) : VideoFrame();
  const auto push_blocked = [&] {
    return block_audio ? encoder.TryPushAudio(pending)
                       : encoder.TryPushVideo(pending);
  };
  REQUIRE(push_blocked());
  REQUIRE(gate.Wait());
  for (int index = 0; index < 3; ++index) {
    pending->pts += block_audio ? 3008 : 3000;
    REQUIRE(push_blocked());
  }
  pending->pts += block_audio ? 3008 : 3000;
  const auto rejected_pts = pending->pts;
  const auto before_rejection = std::chrono::steady_clock::now();
  REQUIRE_FALSE(push_blocked());
  CHECK(std::chrono::steady_clock::now() - before_rejection < 100ms);
  CHECK(pending->pts == rejected_pts);
  CHECK(pending->duration == (block_audio ? 3008 : 3000));
  CHECK(pending->data[0] != nullptr);
  const auto blocked_revision = blocked_signal->GetRevision();
  const auto other_revision = other_signal->GetRevision();

  auto advancing = block_audio ? VideoFrame() : AudioFrame(3008);
  REQUIRE((block_audio ? encoder.TryPushVideo(advancing)
                       : encoder.TryPushAudio(advancing)));
  REQUIRE(capture.WaitStreamPackets(other, 1));
  advancing->pts += block_audio ? 3000 : 3008;
  REQUIRE((block_audio ? encoder.TryPushVideo(advancing)
                       : encoder.TryPushAudio(advancing)));
  REQUIRE(capture.WaitStreamPackets(other, block_audio ? 2 : 4));
  CHECK(blocked_signal->GetRevision() == blocked_revision);
  CHECK(other_signal->GetRevision() != other_revision);
  gate.Release();
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE_FALSE(callback_threads[0].empty());
  REQUIRE_FALSE(callback_threads[1].empty());
  for (const auto& threads : callback_threads) {
    CHECK(std::all_of(threads.begin(), threads.end(),
                      [&](auto id) { return id == threads.front(); }));
  }
  CHECK(callback_threads[0].front() != callback_threads[1].front());
  CHECK(callback_threads[0].size() == (block_audio ? 2 : 4));
  CHECK(callback_threads[1].size() == (block_audio ? 13 : 7));
  const auto streams = encoder.streams();
  CHECK(Decode(streams[0], capture.packets).size() == (block_audio ? 2 : 4));
  const auto audio_frames = Decode(streams[1], capture.packets);
  int decoded_samples = 0;
  for (const auto& frame : audio_frames) decoded_samples += frame->nb_samples;
  CHECK(decoded_samples == (block_audio ? 12288 : 6144));
  for (const auto& stream : streams) {
    std::optional<std::int64_t> last_pts;
    std::optional<std::int64_t> last_dts;
    for (const auto& packet : capture.packets) {
      if (packet->stream_index != stream.stream_index) continue;
      if (last_pts) CHECK(packet->pts > *last_pts);
      if (last_dts) CHECK(packet->dts > *last_dts);
      last_pts = packet->pts;
      last_dts = packet->dts;
    }
  }
  const auto warnings = log.Read();
  CHECK(warnings.empty());
}

TEST_CASE("AvEncoder 写信号支持满队列原帧重试及跨会话重新绑定",
          "[av-encoder][queue][notification][lifecycle]") {
  bool video = true;
  SECTION("视频空间通知") {}
  SECTION("音频空间通知") { video = false; }
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate gate;
  Capture capture;
  auto wake = std::make_shared<WaitSignal>();
  auto original = std::make_shared<WriteSignal>(wake);
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
  encoder.SetWriteSignal(video, original);
  auto config = Config();
  if (video)
    config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  else
    config.video_pixel_format = AV_PIX_FMT_NONE;
  REQUIRE(encoder.Start(config));
  ReleaseGate release_before_encoder{gate};
  auto frame = video ? VideoFrame() : AudioFrame(3008);
  const auto push = [&] {
    return video ? encoder.TryPushVideo(frame) : encoder.TryPushAudio(frame);
  };
  REQUIRE(push());
  REQUIRE(gate.Wait());
  for (int index = 0; index < 3; ++index) {
    frame->pts += frame->duration;
    REQUIRE(push());
  }
  frame->pts += frame->duration;
  const auto rejected_pts = frame->pts;
  const auto* rejected_data = frame->data[0];
  REQUIRE_FALSE(push());
  CHECK(frame->pts == rejected_pts);
  CHECK(frame->data[0] == rejected_data);
  auto replacement = std::make_shared<WriteSignal>(wake);
  encoder.SetWriteSignal(video, replacement);
  original.reset();
  const auto write_revision = replacement->GetRevision();
  const auto wake_revision = wake->GetRevision();
  gate.Release();
  const bool signaled =
      wake->WaitUntil(wake_revision, std::chrono::steady_clock::now() + 1s);
  const bool capacity_changed = replacement->GetRevision() != write_revision;
  bool retried = false;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    const auto revision = wake->GetRevision();
    if (push()) {
      retried = true;
      break;
    }
    if (encoder.error() || !wake->WaitUntil(revision, deadline)) break;
  }
  encoder.Stop();
  CHECK(signaled);
  CHECK(capacity_changed);
  REQUIRE(retried);
  CHECK_FALSE(encoder.error());
  CHECK_FALSE(capture.error);
  CHECK(capture.packets.size() == (video ? 5 : 16));
  const auto decoded = Decode(encoder.streams().front(), capture.packets);
  CHECK(decoded.size() == (video ? 5 : 15));
  CHECK(frame->pts == rejected_pts);
  CHECK_FALSE(push());
  capture.packets.clear();
  const auto restart_revision = wake->GetRevision();
  REQUIRE(encoder.Start(config));
  CHECK(
      wake->WaitUntil(restart_revision, std::chrono::steady_clock::now() + 1s));
  REQUIRE(push());
  encoder.Stop();
  CHECK(capture.packets.size() == (video ? 1 : 4));
  CHECK(Decode(encoder.streams().front(), capture.packets).size() ==
        (video ? 1 : 3));
  encoder.SetWriteSignal(video, {});
  replacement.reset();
  CHECK_FALSE(encoder.error());
}

TEST_CASE("AvEncoder 双轨并发失败仅通知一次并关闭两轨准入",
          "[av-encoder][error][parallel]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate callbacks;
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  encoder.SetOnPacket([&](const ffmpeg::Packet&) {
    callbacks.Enter();
    throw std::runtime_error("测试并发 packet 回调失败");
  });
  REQUIRE(encoder.Start(Config()));
  ReleaseGate release_before_encoder{callbacks};
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
  REQUIRE(encoder.TryPushAudio(AudioFrame(3008)));
  REQUIRE(callbacks.WaitBoth());
  callbacks.Release();
  REQUIRE(capture.WaitError());
  encoder.Stop();
  CHECK(capture.error_count == 1);
  REQUIRE(encoder.error());
  CHECK_FALSE(encoder.TryPushVideo(VideoFrame()));
  CHECK_FALSE(encoder.TryPushAudio(AudioFrame(3008)));
  encoder.Stop();
  CHECK(capture.error_count == 1);
}

TEST_CASE("AvEncoder Stop 排空两轨各自队列和音频短尾并支持重新启动",
          "[av-encoder][stop][drain][parallel]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Gate video_callback;
  Gate audio_callback;
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet) {
    {
      std::lock_guard lock(capture.mutex);
      capture.packets.push_back(packet.Ref());
      capture.wake.notify_all();
    }
    (packet->stream_index == 0 ? video_callback : audio_callback).Enter();
  });
  CHECK_FALSE(encoder.TryPushVideo(VideoFrame()));
  CHECK_FALSE(encoder.TryPushAudio(AudioFrame(3008)));
  REQUIRE(encoder.Start(Config()));
  ReleaseGate release_video{video_callback};
  ReleaseGate release_audio{audio_callback};
  auto video = VideoFrame();
  auto audio = AudioFrame(3008);
  REQUIRE(encoder.TryPushVideo(video));
  REQUIRE(encoder.TryPushAudio(audio));
  REQUIRE(video_callback.Wait());
  REQUIRE(audio_callback.Wait());
  for (int index = 0; index < 3; ++index) {
    video->pts += 3000;
    audio->pts += 3008;
    REQUIRE(encoder.TryPushVideo(video));
    REQUIRE(encoder.TryPushAudio(audio));
  }
  CHECK_FALSE(encoder.TryPushVideo(video));
  CHECK_FALSE(encoder.TryPushAudio(audio));
  auto stopping = std::async(std::launch::async, [&] { encoder.Stop(); });
  ReleaseGate release_video_before_future{video_callback};
  ReleaseGate release_audio_before_future{audio_callback};
  CHECK(stopping.wait_for(50ms) == std::future_status::timeout);
  CHECK_FALSE(encoder.TryPushVideo(video));
  CHECK_FALSE(encoder.TryPushAudio(audio));
  video_callback.Release();
  CHECK(stopping.wait_for(50ms) == std::future_status::timeout);
  audio_callback.Release();
  REQUIRE(stopping.wait_for(10s) == std::future_status::ready);
  stopping.get();
  CHECK_FALSE(encoder.error());
  auto streams = encoder.streams();
  CHECK(Decode(streams[0], capture.packets).size() == 4);
  const auto audio_frames = Decode(streams[1], capture.packets);
  int decoded_samples = 0;
  for (const auto& frame : audio_frames) decoded_samples += frame->nb_samples;
  CHECK(decoded_samples == 12288);
  REQUIRE_FALSE(capture.packets.empty());
  for (auto it = capture.packets.rbegin(); it != capture.packets.rend(); ++it) {
    if ((*it)->stream_index != 1) continue;
    CHECK((*it)->duration == 768);
    break;
  }
  capture.packets.clear();
  REQUIRE(encoder.Start(Config()));
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
  REQUIRE(encoder.TryPushAudio(AudioFrame(3008)));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  streams = encoder.streams();
  CHECK(Decode(streams[0], capture.packets).size() == 1);
  CHECK(Decode(streams[1], capture.packets).size() == 3);
  CHECK(capture.error_count == 0);
}

TEST_CASE("AvEncoder 启用期间拒绝向禁用轨道提交且不影响启用轨道",
          "[av-encoder][configuration][validation]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  SECTION("只启用视频") {
    config.audio_sample_format = AV_SAMPLE_FMT_NONE;
    REQUIRE(encoder.Start(config));
    CHECK_THROWS_AS(encoder.TryPushAudio(AudioFrame(3008)),
                    std::invalid_argument);
    REQUIRE(encoder.TryPushVideo(VideoFrame()));
  }
  SECTION("只启用音频") {
    config.video_pixel_format = AV_PIX_FMT_NONE;
    REQUIRE(encoder.Start(config));
    CHECK_THROWS_AS(encoder.TryPushVideo(VideoFrame()), std::invalid_argument);
    REQUIRE(encoder.TryPushAudio(AudioFrame(3008)));
  }
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK(capture.error_count == 0);
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
  SECTION("视频帧率零分子") { invalid.fps = {0, 1}; }
  SECTION("视频帧率零分母") { invalid.fps = {30, 0}; }
  SECTION("视频帧率为负数") { invalid.fps = {-30, 1}; }
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
  CHECK_FALSE(encoder.TryPushVideo(VideoFrame()));
  encoder.Stop();
  CHECK(capture.error_count == 0);
  CHECK(capture.packets.empty());
  auto valid = Config();
  valid.audio_sample_format = AV_SAMPLE_FMT_NONE;
  REQUIRE(encoder.Start(valid));
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
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
  SECTION("视频帧格式不匹配") { frame->width = 32; }
  SECTION("packet 消费者抛异常") {
    encoder.SetOnPacket([](const ffmpeg::Packet&) {
      throw std::runtime_error("测试 packet 消费异常");
    });
  }
  REQUIRE(encoder.Start(config));
  REQUIRE(encoder.TryPushVideo(frame));
  REQUIRE(capture.WaitError());
  encoder.Stop();
  CHECK(capture.error_count == 1);
  REQUIRE(capture.error);
  REQUIRE(encoder.error());
  CHECK_THROWS(std::rethrow_exception(capture.error));
  CHECK_FALSE(encoder.TryPushVideo(VideoFrame()));
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
  REQUIRE(encoder.TryPushVideo(cpu));
  REQUIRE(capture.WaitPackets(1));
  auto gpu = GpuFrame(*device, AV_PIX_FMT_NV12, config.width, config.height);
  const auto* pool =
      reinterpret_cast<const AVHWFramesContext*>(gpu->hw_frames_ctx->data);
  REQUIRE(pool->device_ref->data == device->get()->data);
  gpu->pts = 93000;
  REQUIRE(encoder.TryPushVideo(gpu));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  REQUIRE(capture.packets.size() == 2);
  CHECK(capture.packets[0]->pts == 30);
  CHECK(capture.packets[1]->pts == 31);
  const auto frames = Decode(streams[0], capture.packets);
  REQUIRE(frames.size() == 2);
  for (const auto& frame : frames) {
    CHECK(frame->width == config.width);
    CHECK(frame->height == config.height);
    CHECK(frame->data[0][0] >= 76);
    CHECK(frame->data[0][0] <= 84);
  }
  CHECK(cpu->pts == 90000);
  CHECK(gpu->pts == 93000);
  CHECK(cpu->duration == 3000);
  CHECK(gpu->duration == 3000);
}

TEST_CASE("AvEncoder 自动下载 GPU 帧后送入 CPU 编码器",
          "[av-encoder][gpu][download]") {
  auto device = CudaDevice();
  if (!device) {
    SKIP("当前环境不能创建 CUDA 设备");
  }
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  Capture capture;
  AvEncoder encoder(cpu);
  capture.Bind(encoder);
  auto config = Config();
  config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  config.video_encoder_name = "libx264";
  REQUIRE(encoder.Start(config));
  const auto streams = encoder.streams();
  REQUIRE(streams.size() == 1);
  CHECK(streams[0].codec_parameters.get()->format == AV_PIX_FMT_NV12);
  const auto gpu =
      GpuFrame(*device, AV_PIX_FMT_NV12, config.width, config.height);
  const auto* gpu_data = gpu->data[0];
  const auto* gpu_pool = gpu->hw_frames_ctx;
  REQUIRE(encoder.TryPushVideo(gpu));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK_FALSE(capture.error);
  CHECK(capture.error_count == 0);
  REQUIRE(capture.packets.size() == 1);
  CHECK(capture.packets[0]->pts == 30);
  CHECK(capture.packets[0]->duration == 1);
  const auto frames = Decode(streams[0], capture.packets);
  REQUIRE(frames.size() == 1);
  const auto& decoded = frames[0];
  CHECK(decoded->width == config.width);
  CHECK(decoded->height == config.height);
  CHECK(decoded->pts == 30);
  REQUIRE(decoded->format == AV_PIX_FMT_YUV420P);
  for (int row = 0; row < config.height; ++row) {
    const auto* pixels = decoded->data[0] + row * decoded->linesize[0];
    CHECK(std::all_of(pixels, pixels + config.width, [](std::uint8_t value) {
      return value >= 76 && value <= 84;
    }));
  }
  CHECK(decoded->data[1][0] >= 124);
  CHECK(decoded->data[1][0] <= 132);
  CHECK(decoded->data[2][0] >= 124);
  CHECK(decoded->data[2][0] <= 132);
  CHECK(gpu->format == AV_PIX_FMT_CUDA);
  CHECK(gpu->data[0] == gpu_data);
  CHECK(gpu->hw_frames_ctx == gpu_pool);
  CHECK(gpu->pts == 90000);
  CHECK(gpu->duration == 3000);
  CHECK(gpu->time_base.num == 1);
  CHECK(gpu->time_base.den == 90000);
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
  auto incompatible =
      GpuFrame(*device, AV_PIX_FMT_YUV420P, config.width, config.height);
  REQUIRE(encoder.TryPushVideo(incompatible));
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
  REQUIRE(encoder.TryPushVideo(VideoFrame()));
  encoder.Stop();
  CHECK_FALSE(encoder.error());
  CHECK(Decode(streams[0], capture.packets).size() == 1);
}

}  // namespace

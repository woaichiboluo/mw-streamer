#include "mw/streamer/writer/video_writer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"
#include "mw/streamer/ffmpeg/packet.h"
#include "mw/streamer/init/init.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
namespace fs = std::filesystem;
using mw::streamer::AvEncoderConfig;
using mw::streamer::VideoWriter;
using Runtime = std::unique_ptr<mw::streamer::MwStreamerContext,
                                decltype(&mw::streamer::Shutdown)>;

Runtime InitRuntime() {
  mw::streamer::InitConfig config;
  config.event_poller_threads = 1;
  config.work_threads = 1;
  config.enable_cpu_affinity = false;
  config.log.console.enabled = false;
  return Runtime(mw::streamer::Init(config), &mw::streamer::Shutdown);
}

class OutputDirectory final {
 public:
  OutputDirectory()
      : root_(fs::absolute(fs::current_path()).lexically_normal()),
        path_(root_ /
              ("video-writer-" + std::to_string(std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count()))) {
    fs::create_directory(path_);
  }
  ~OutputDirectory() {
    if (path_.parent_path() == root_) {
      std::error_code error;
      fs::remove_all(path_, error);
    }
  }
  OutputDirectory(const OutputDirectory&) = delete;
  OutputDirectory& operator=(const OutputDirectory&) = delete;

  std::string File(const std::string& name = "result.mp4") const {
    return (path_ / fs::u8path(name)).generic_u8string();
  }

 private:
  fs::path root_;
  fs::path path_;
};

AvEncoderConfig Config(bool audio = false) {
  AvEncoderConfig config;
  config.width = 64;
  config.height = 64;
  config.fps = {25, 1};
  config.video_options = {{"preset", "ultrafast"}, {"threads", "1"}};
  if (!audio) config.audio_sample_format = AV_SAMPLE_FMT_NONE;
  return config;
}

ffmpeg::Frame VideoFrame(std::int64_t pts, int width = 64, int height = 64) {
  ffmpeg::Frame frame;
  frame->format = AV_PIX_FMT_NV12;
  frame->width = width;
  frame->height = height;
  frame->pts = pts;
  frame->pkt_dts = pts;
  frame->best_effort_timestamp = pts;
  frame->time_base = {1, 90000};
  frame->duration = 3600;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 writer 视频测试帧");
  for (int plane = 0; plane < 2; ++plane) {
    for (int row = 0; row < (plane == 0 ? height : height / 2); ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane],
                  plane == 0 ? 80 : 128, static_cast<std::size_t>(width));
    }
  }
  return frame;
}

ffmpeg::Frame AudioFrame(std::int64_t pts) {
  // A decoded format different from the writer's 48 kHz planar stereo target.
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_S16;
  frame->sample_rate = 44100;
  frame->nb_samples = 441;
  av_channel_layout_default(&frame->ch_layout, 1);
  frame->pts = pts;
  frame->pkt_dts = pts;
  frame->best_effort_timestamp = pts;
  frame->time_base = {1, 44100};
  frame->duration = 441;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 writer 音频测试帧");
  auto* samples = reinterpret_cast<std::int16_t*>(frame->data[0]);
  std::fill_n(samples, frame->nb_samples, std::int16_t{8192});
  return frame;
}

struct FormatCloser {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct DecodedFile {
  std::vector<double> video_times;
  std::vector<double> video_packet_pts;
  bool reordered = false;
  int audio_sample_rate = 0;
  int audio_channels = 0;
  int decoded_audio_samples = 0;
  double first_audible_time = std::numeric_limits<double>::infinity();
};

DecodedFile DecodeFile(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开 writer 输出文件");
  std::unique_ptr<AVFormatContext, FormatCloser> format(raw);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "读取 writer 输出轨道");
  std::vector<std::unique_ptr<ffmpeg::CodecContext>> decoders(raw->nb_streams);
  for (unsigned index = 0; index < raw->nb_streams; ++index) {
    auto* stream = raw->streams[index];
    const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("找不到 writer 输出解码器");
    decoders[index] = std::make_unique<ffmpeg::CodecContext>(codec);
    auto* context = decoders[index]->get();
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(context, stream->codecpar),
        "配置 writer 输出解码器");
    context->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(context, codec, nullptr), "打开 writer 输出解码器");
  }
  DecodedFile output;
  const auto receive = [&](unsigned index) {
    auto* decoder = decoders[index]->get();
    for (;;) {
      ffmpeg::Frame frame;
      const int result = avcodec_receive_frame(decoder, frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(result, "解码 writer 输出帧");
      const auto pts = frame->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) throw std::runtime_error("输出帧缺少时间戳");
      const double time =
          static_cast<double>(pts) * av_q2d(raw->streams[index]->time_base);
      if (decoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        output.video_times.push_back(time);
      } else if (decoder->codec_type == AVMEDIA_TYPE_AUDIO) {
        output.audio_sample_rate = frame->sample_rate;
        output.audio_channels = frame->ch_layout.nb_channels;
        output.decoded_audio_samples += frame->nb_samples;
        if (frame->format != AV_SAMPLE_FMT_FLTP)
          throw std::runtime_error("AAC 测试解码格式不是 planar float");
        const auto* samples =
            reinterpret_cast<const float*>(frame->extended_data[0]);
        for (int sample = 0; sample < frame->nb_samples; ++sample) {
          if (std::abs(samples[sample]) > 0.05F) {
            output.first_audible_time = std::min(
                output.first_audible_time,
                time + static_cast<double>(sample) / frame->sample_rate);
            break;
          }
        }
      }
    }
  };
  ffmpeg::Packet packet;
  int result = 0;
  while ((result = av_read_frame(raw, packet.get())) >= 0) {
    const auto index = static_cast<unsigned>(packet->stream_index);
    if (raw->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      output.video_packet_pts.push_back(static_cast<double>(packet->pts) *
                                        av_q2d(raw->streams[index]->time_base));
      output.reordered = output.reordered || packet->pts != packet->dts;
    }
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoders[index]->get(), packet.get()),
        "提交 writer 输出解码包");
    receive(index);
    packet.Unref();
  }
  ffmpeg::FfmpegException::throwIfError(result == AVERROR_EOF ? 0 : result,
                                        "读取 writer 输出包");
  for (unsigned index = 0; index < raw->nb_streams; ++index) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoders[index]->get(), nullptr),
        "排空 writer 输出解码器");
    receive(index);
  }
  return output;
}

TEST_CASE("VideoWriter 保存全部视频帧并保留缺口与编码重排",
          "[writer][timing][drain]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  auto config = Config();
  config.max_b_frames = 2;
  config.video_options["preset"] = "medium";
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  writer.Open(directory.File(), config);
  CHECK_FALSE(writer.source_start_time());
  std::vector<double> expected;
  for (int index = 0; index < 12; ++index) {
    const auto offset = static_cast<std::int64_t>(index + (index >= 3 ? 2 : 0));
    auto frame = VideoFrame(9000000 + offset * 3600);
    const auto before = frame.Ref();
    writer.WriteVideo(frame);
    CHECK(frame->pts == before->pts);
    CHECK(frame->pkt_dts == before->pkt_dts);
    CHECK(frame->best_effort_timestamp == before->best_effort_timestamp);
    CHECK(frame->duration == before->duration);
    CHECK(frame->time_base.num == 1);
    CHECK(frame->time_base.den == 90000);
    CHECK(frame->data[0][0] == 80);
    expected.push_back(static_cast<double>(offset) / 25.0);
  }
  const auto origin = writer.source_start_time();
  REQUIRE(origin);
  CHECK(av_compare_ts(origin->pts, origin->time_base, 100, AVRational{1, 1}) ==
        0);
  REQUIRE_NOTHROW(writer.Finish());
  CHECK_NOTHROW(writer.Finish());
  const auto decoded = DecodeFile(directory.File());
  REQUIRE(decoded.video_times.size() == expected.size());
  CHECK(decoded.reordered);
  for (std::size_t index = 0; index < expected.size(); ++index)
    CHECK(std::abs(decoded.video_times[index] - expected[index]) < 0.0001);
}

TEST_CASE("VideoWriter 首帧到达顺序不改变音画偏移并转换解码音频",
          "[writer][timing][audio]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  writer.Open(directory.File(), Config(true));
  // Audio arrives first and can exceed the encoder queue's three-frame size
  // before the video origin is known. A serial caller must still make progress.
  for (int index = 0; index < 4; ++index) {
    auto audio = AudioFrame(4410000 + 3528 + index * 441);
    writer.WriteAudio(audio);
    CHECK(audio->pts == 4410000 + 3528 + index * 441);
    CHECK(audio->sample_rate == 44100);
    CHECK(audio->format == AV_SAMPLE_FMT_S16);
    CHECK(audio->ch_layout.nb_channels == 1);
  }
  CHECK_FALSE(writer.source_start_time());
  for (int index = 0; index < 12; ++index) {
    writer.WriteVideo(VideoFrame(9000000 + index * 3600));
    for (int part = 0; part < 3; ++part) {
      const auto audio_index = 4 + index * 3 + part;
      writer.WriteAudio(AudioFrame(4410000 + 3528 + audio_index * 441));
    }
  }
  const auto origin = writer.source_start_time();
  REQUIRE(origin);
  CHECK(av_compare_ts(origin->pts, origin->time_base, 100, AVRational{1, 1}) ==
        0);
  writer.Finish();
  const auto decoded = DecodeFile(directory.File());
  REQUIRE(decoded.video_times.size() == 12);
  CHECK(std::abs(decoded.video_times.front()) < 0.0001);
  CHECK(decoded.audio_sample_rate == 48000);
  CHECK(decoded.audio_channels == 2);
  CHECK(decoded.decoded_audio_samples >= 19200);
  // AAC priming/padding are not an independent per-track zeroing policy.
  CHECK(decoded.first_audible_time > 0.05);
  CHECK(decoded.first_audible_time < 0.11);
}

TEST_CASE("VideoWriter 非视频帧格的来源起点仍输出零起点",
          "[writer][timing][origin]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  writer.Open(directory.File(), Config());
  // 100.01 seconds is not an integer multiple of the 1/25 encoder time base.
  for (int index = 0; index < 6; ++index)
    writer.WriteVideo(VideoFrame(9000900 + index * 3600));
  const auto origin = writer.source_start_time();
  REQUIRE(origin);
  CHECK(av_compare_ts(origin->pts, origin->time_base, 10001,
                      AVRational{1, 100}) == 0);
  writer.Finish();
  const auto decoded = DecodeFile(directory.File());
  REQUIRE(decoded.video_times.size() == 6);
  for (std::size_t index = 0; index < decoded.video_times.size(); ++index)
    CHECK(std::abs(decoded.video_times[index] -
                   static_cast<double>(index) / 25.0) < 0.0001);
}

TEST_CASE("VideoWriter 音频早于视频时保留反向偏移而不按到达顺序归零",
          "[writer][timing][audio][origin]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  writer.Open(directory.File(), Config(true));
  // Deliver the later source track first; the earliest presentation is audio.
  for (int index = 0; index < 4; ++index)
    writer.WriteVideo(VideoFrame(9007200 + index * 3600));
  CHECK_FALSE(writer.source_start_time());
  for (int index = 0; index < 40; ++index) {
    writer.WriteAudio(AudioFrame(4410000 + index * 441));
    if (index < 8) writer.WriteVideo(VideoFrame(9007200 + (index + 4) * 3600));
  }
  const auto origin = writer.source_start_time();
  REQUIRE(origin);
  CHECK(av_compare_ts(origin->pts, origin->time_base, 100, AVRational{1, 1}) ==
        0);
  writer.Finish();
  const auto decoded = DecodeFile(directory.File());
  REQUIRE(decoded.video_times.size() == 12);
  for (std::size_t index = 0; index < decoded.video_times.size(); ++index)
    CHECK(std::abs(decoded.video_times[index] -
                   (0.08 + static_cast<double>(index) / 25.0)) < 0.0001);
  CHECK(decoded.first_audible_time < 0.03);
  CHECK(decoded.decoded_audio_samples >= 19200);
}

TEST_CASE("VideoWriter 支持纯音频并排空不足一个 AAC 帧的尾样本",
          "[writer][audio][drain]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  auto config = Config(true);
  config.video_pixel_format = AV_PIX_FMT_NONE;
  writer.Open(directory.File(), config);
  writer.WriteAudio(AudioFrame(4410000));
  writer.Finish();
  const auto decoded = DecodeFile(directory.File());
  CHECK(decoded.video_times.empty());
  CHECK(decoded.audio_sample_rate == 48000);
  CHECK(decoded.decoded_audio_samples >= 480);
  CHECK(decoded.first_audible_time < 0.03);
}

TEST_CASE("VideoWriter 明确报告空任务缺少轨道取消和打开失败",
          "[writer][lifecycle][error]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  SECTION("未打开不能写入") {
    CHECK_THROWS(writer.WriteVideo(VideoFrame(9000000)));
  }
  SECTION("拒绝缺少来源 PTS 的帧") {
    writer.Open(directory.File(), Config());
    auto frame = VideoFrame(9000000);
    frame->pts = AV_NOPTS_VALUE;
    CHECK_THROWS_AS(writer.WriteVideo(frame), std::invalid_argument);
    CHECK_FALSE(writer.source_start_time());
    CHECK_THROWS_AS(writer.WriteVideo(VideoFrame(9000000)),
                    std::invalid_argument);
    CHECK_FALSE(writer.source_start_time());
    CHECK_THROWS_AS(writer.Finish(), std::invalid_argument);
  }
  SECTION("来源 PTS 回退是终止错误且不改变已确定起点") {
    writer.Open(directory.File(), Config());
    writer.WriteVideo(VideoFrame(9000000));
    CHECK_THROWS_AS(writer.WriteVideo(VideoFrame(8996400)),
                    std::invalid_argument);
    const auto origin = writer.source_start_time();
    REQUIRE(origin);
    CHECK(av_compare_ts(origin->pts, origin->time_base, 100,
                        AVRational{1, 1}) == 0);
    CHECK_THROWS_AS(writer.WriteVideo(VideoFrame(9003600)),
                    std::invalid_argument);
    CHECK_THROWS_AS(writer.Finish(), std::invalid_argument);
  }
  SECTION("空任务不能报告完成") {
    writer.Open(directory.File(), Config());
    CHECK_THROWS(writer.Finish());
  }
  SECTION("双轨任务缺少音频首帧不能报告完成") {
    writer.Open(directory.File(), Config(true));
    writer.WriteVideo(VideoFrame(9000000));
    CHECK_THROWS(writer.Finish());
  }
  SECTION("取消后唤醒并拒绝后续写入与完成") {
    writer.Open(directory.File(), Config());
    writer.RequestStop();
    CHECK_THROWS(writer.WriteVideo(VideoFrame(9000000)));
    CHECK_THROWS(writer.Finish());
  }
  SECTION("父目录不存在时打开失败") {
    CHECK_THROWS(writer.Open(directory.File("missing/result.mp4"), Config()));
  }
  SECTION("禁止在活跃会话上重复打开") {
    writer.Open(directory.File(), Config());
    CHECK_THROWS_AS(writer.Open(directory.File("another.mp4"), Config()),
                    std::logic_error);
  }
}

TEST_CASE("VideoWriter Finish 报告后台编码器的视频布局失败",
          "[writer][error][drain]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  writer.Open(directory.File(), Config());
  // Device/layout validation runs on AvEncoder's worker, so WriteVideo may
  // observe it immediately or return before the worker publishes the error.
  bool reported_by_write = false;
  try {
    writer.WriteVideo(VideoFrame(9000000, 32, 64));
  } catch (const std::invalid_argument&) {
    reported_by_write = true;
  }
  INFO("WriteVideo 已观察到后台错误: " << reported_by_write);
  CHECK_THROWS_AS(writer.Finish(), std::invalid_argument);
  CHECK_THROWS_AS(writer.Finish(), std::invalid_argument);
}

TEST_CASE("VideoWriter 跨线程取消正在连续阻塞提交的调用方",
          "[writer][cancel][backpressure]") {
  const auto runtime = InitRuntime();
  OutputDirectory directory;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  VideoWriter writer(cpu);
  writer.Open(directory.File(), Config());
  writer.WriteVideo(VideoFrame(9000000));
  std::promise<void> started;
  auto ready = started.get_future();
  auto producer = std::async(std::launch::async, [&] {
    started.set_value();
    try {
      for (std::int64_t index = 1;; ++index)
        writer.WriteVideo(VideoFrame(9000000 + index * 3600));
    } catch (const std::runtime_error&) {
      return true;
    }
  });
  const auto started_status = ready.wait_for(std::chrono::seconds(5));
  writer.RequestStop();
  REQUIRE(started_status == std::future_status::ready);
  REQUIRE(producer.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  CHECK(producer.get());
  CHECK_THROWS(writer.Finish());
}

struct BufferUnref {
  void operator()(AVBufferRef* buffer) const { av_buffer_unref(&buffer); }
};

TEST_CASE("VideoWriter 复用 CUDA 设备编码硬件帧并保留来源属性",
          "[writer][cuda][timing][drain]") {
  const auto runtime = InitRuntime();
  std::unique_ptr<ffmpeg::HwDeviceContext> device;
  try {
    device =
        std::make_unique<ffmpeg::HwDeviceContext>(ffmpeg::HwDeviceType::kCuda);
  } catch (const std::exception& error) {
    SKIP("CUDA 设备不可用: " << error.what());
  }
  OutputDirectory directory;
  VideoWriter writer(*device);
  auto config = Config();
  config.width = 256;
  config.height = 256;
  config.video_encoder_name = "h264_nvenc";
  config.video_options = {{"preset", "p1"}};
  writer.Open(directory.File(), config);
  std::unique_ptr<AVBufferRef, BufferUnref> pool(
      av_hwframe_ctx_alloc(device->get()));
  REQUIRE(pool);
  auto* native = reinterpret_cast<AVHWFramesContext*>(pool->data);
  native->format = AV_PIX_FMT_CUDA;
  native->sw_format = AV_PIX_FMT_NV12;
  native->width = 256;
  native->height = 256;
  native->initial_pool_size = 4;
  ffmpeg::FfmpegException::throwIfError(av_hwframe_ctx_init(pool.get()),
                                        "初始化 writer CUDA 测试帧池");
  for (int index = 0; index < 8; ++index) {
    const auto source = VideoFrame(9000000 + index * 3600, 256, 256);
    ffmpeg::Frame frame;
    ffmpeg::FfmpegException::throwIfError(
        av_hwframe_get_buffer(pool.get(), frame.get(), 0),
        "分配 writer CUDA 测试帧");
    ffmpeg::FfmpegException::throwIfError(
        av_hwframe_transfer_data(frame.get(), source.get(), 0),
        "上传 writer CUDA 测试帧");
    frame.CopyPropertiesFrom(source);
    writer.WriteVideo(frame);
    CHECK(frame->pts == source->pts);
    CHECK(frame->time_base.den == 90000);
    CHECK(frame->format == AV_PIX_FMT_CUDA);
  }
  writer.Finish();
  const auto decoded = DecodeFile(directory.File());
  REQUIRE(decoded.video_times.size() == 8);
  for (std::size_t index = 0; index < decoded.video_times.size(); ++index)
    CHECK(std::abs(decoded.video_times[index] -
                   static_cast<double>(index) / 25.0) < 0.0001);
}

}  // namespace

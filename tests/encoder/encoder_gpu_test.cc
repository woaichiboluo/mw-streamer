#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mw/streamer/encoder/encoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "mw/streamer/ffmpeg/decoder.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/init/init.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using namespace std::chrono_literals;
constexpr AVRational kNanoseconds{1, 1000000000};
constexpr std::int64_t kStartNs = 23000000000;
constexpr std::int64_t kFrameDurationNs = 100000000;

struct CloseInput {
  void operator()(AVFormatContext* input) const {
    avformat_close_input(&input);
  }
};

struct ShutdownRuntime {
  void operator()(mw::streamer::MwStreamerContext* context) const {
    mw::streamer::Shutdown(context);
  }
};

std::vector<ffmpeg::Frame> ReceiveFrames(ffmpeg::Decoder& decoder) {
  std::vector<ffmpeg::Frame> frames;
  ffmpeg::Frame frame;
  while (decoder.ReceiveFrame(frame) == ffmpeg::DecodeResult::kFrame) {
    frames.push_back(frame.Ref());
  }
  return frames;
}

void AppendFrames(std::vector<ffmpeg::Frame>& destination,
                  std::vector<ffmpeg::Frame> source) {
  for (auto& frame : source) destination.push_back(std::move(frame));
}

std::vector<ffmpeg::Frame> ReadFrames(const ffmpeg::HwDeviceContext& device,
                                      ffmpeg::StreamInfo& stream) {
  AVFormatContext* raw_input = nullptr;
  const std::string path =
      std::string(MW_STREAMER_ENCODER_TEST_DATA_DIR) + "/h265_cuda.mp4";
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw_input, path.c_str(), nullptr, nullptr),
      "打开编码迁移测试文件");
  std::unique_ptr<AVFormatContext, CloseInput> input(raw_input);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(input.get(), nullptr), "读取编码迁移测试轨道");
  const int stream_index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  ffmpeg::FfmpegException::throwIfError(stream_index, "查找编码迁移测试视频");
  const auto* source = input->streams[stream_index];
  stream = {stream_index, ffmpeg::CodecParameters(*source->codecpar),
            source->time_base};
  ffmpeg::VideoDecoder decoder(stream, device, "hevc");
  std::vector<ffmpeg::Frame> frames;
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "读取编码迁移测试包");
    if (packet->stream_index != stream_index) continue;
    while (!decoder.SendPacket(packet)) {
      AppendFrames(frames, ReceiveFrames(decoder));
    }
    AppendFrames(frames, ReceiveFrames(decoder));
  }
  while (!decoder.Drain()) AppendFrames(frames, ReceiveFrames(decoder));
  AppendFrames(frames, ReceiveFrames(decoder));
  REQUIRE(frames.size() >= 20);
  frames.resize(20);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    REQUIRE(frames[index]->width == 256);
    REQUIRE(frames[index]->height == 144);
    if (device.get()) {
      REQUIRE(frames[index]->format == AV_PIX_FMT_CUDA);
      REQUIRE(frames[index]->hw_frames_ctx != nullptr);
      const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
          frames[index]->hw_frames_ctx->data);
      REQUIRE(pool->sw_format == AV_PIX_FMT_NV12);
      REQUIRE(pool->device_ref->data == device.get()->data);
    } else {
      REQUIRE(frames[index]->format == AV_PIX_FMT_YUV420P);
      REQUIRE(frames[index]->hw_frames_ctx == nullptr);
    }
    frames[index]->time_base = kNanoseconds;
    frames[index]->pts =
        kStartNs + static_cast<std::int64_t>(index) * kFrameDurationNs;
    frames[index]->duration = kFrameDurationNs;
  }
  return frames;
}

ffmpeg::Frame Download(const ffmpeg::Frame& source) {
  ffmpeg::Frame result;
  ffmpeg::FfmpegException::throwIfError(
      av_hwframe_transfer_data(result.get(), source.get(), 0),
      "下载编码迁移测试参考帧");
  ffmpeg::FfmpegException::throwIfError(
      av_frame_copy_props(result.get(), source.get()), "复制编码测试帧属性");
  REQUIRE(result->format == AV_PIX_FMT_NV12);
  return result;
}

std::array<double, 3> PixelError(const ffmpeg::Frame& source,
                                 const ffmpeg::Frame& decoded) {
  REQUIRE((source->format == AV_PIX_FMT_NV12 ||
           source->format == AV_PIX_FMT_YUV420P));
  REQUIRE((decoded->format == AV_PIX_FMT_YUV420P ||
           decoded->format == AV_PIX_FMT_YUVJ420P));
  REQUIRE(source->width == decoded->width);
  REQUIRE(source->height == decoded->height);
  std::array<double, 3> errors{};
  for (int plane = 0; plane < 3; ++plane) {
    const auto plane_index = static_cast<std::size_t>(plane);
    const int width = plane == 0 ? source->width : (source->width + 1) / 2;
    const int height = plane == 0 ? source->height : (source->height + 1) / 2;
    const bool interleaved = source->format == AV_PIX_FMT_NV12;
    const int source_plane = interleaved && plane > 0 ? 1 : plane;
    for (int row = 0; row < height; ++row) {
      const auto* expected =
          source->data[source_plane] + row * source->linesize[source_plane];
      const auto* actual =
          decoded->data[plane] + row * decoded->linesize[plane];
      for (int column = 0; column < width; ++column) {
        const int source_column =
            interleaved && plane > 0 ? 2 * column + plane - 1 : column;
        const int difference = expected[source_column] - actual[column];
        errors[plane_index] += difference * difference;
      }
    }
    errors[plane_index] /= width * height;
  }
  return errors;
}

std::uint64_t PixelHash(const ffmpeg::Frame& frame) {
  REQUIRE((frame->format == AV_PIX_FMT_NV12 ||
           frame->format == AV_PIX_FMT_YUV420P));
  std::uint64_t hash = 14695981039346656037ULL;
  const bool interleaved = frame->format == AV_PIX_FMT_NV12;
  for (int plane = 0; plane < (interleaved ? 2 : 3); ++plane) {
    const int rows = plane == 0 ? frame->height : (frame->height + 1) / 2;
    const int columns =
        plane == 0 || interleaved ? frame->width : (frame->width + 1) / 2;
    for (int row = 0; row < rows; ++row) {
      const auto* data = frame->data[plane] + row * frame->linesize[plane];
      for (int column = 0; column < columns; ++column) {
        hash = (hash ^ data[column]) * 1099511628211ULL;
      }
    }
  }
  return hash;
}

struct FrameSnapshot {
  int format;
  std::int64_t pts;
  std::int64_t duration;
  AVRational time_base;
  std::array<const std::uint8_t*, 3> data;
  const std::uint8_t* pool;
  const std::uint8_t* device;
  AVColorRange color_range;
  AVColorSpace color_space;
};

FrameSnapshot Snapshot(const ffmpeg::Frame& frame) {
  const auto* pool = frame->hw_frames_ctx;
  const auto* context =
      pool ? reinterpret_cast<const AVHWFramesContext*>(pool->data) : nullptr;
  return {frame->format,
          frame->pts,
          frame->duration,
          frame->time_base,
          {frame->data[0], frame->data[1], frame->data[2]},
          pool ? pool->data : nullptr,
          context ? context->device_ref->data : nullptr,
          frame->color_range,
          frame->colorspace};
}

void CheckUnchanged(const ffmpeg::Frame& frame, const FrameSnapshot& before) {
  const auto after = Snapshot(frame);
  CHECK(after.format == before.format);
  CHECK(after.pts == before.pts);
  CHECK(after.duration == before.duration);
  CHECK(av_cmp_q(after.time_base, before.time_base) == 0);
  CHECK(after.data == before.data);
  CHECK(after.pool == before.pool);
  CHECK(after.device == before.device);
  CHECK(after.color_range == before.color_range);
  CHECK(after.color_space == before.color_space);
}

struct Capture {
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<ffmpeg::StreamInfo> streams;
  std::vector<ffmpeg::Packet> packets;
  std::vector<std::int64_t> dts_ns;
  std::string error;
  int ended = 0;
};

std::vector<ffmpeg::Frame> DecodePackets(
    const ffmpeg::StreamInfo& stream,
    const std::vector<ffmpeg::Packet>& packets) {
  ffmpeg::VideoDecoder decoder(stream);
  std::vector<ffmpeg::Frame> frames;
  for (const auto& packet : packets) {
    while (!decoder.SendPacket(packet)) {
      AppendFrames(frames, ReceiveFrames(decoder));
    }
    AppendFrames(frames, ReceiveFrames(decoder));
  }
  while (!decoder.Drain()) AppendFrames(frames, ReceiveFrames(decoder));
  AppendFrames(frames, ReceiveFrames(decoder));
  return frames;
}

TEST_CASE("软硬解码交叉编码机械上传下载并保留共享CUDA设备和输入帧",
          "[.][encoder][cuda]") {
  const std::string scenario =
      GENERATE("cpu_to_cpu", "gpu_to_cpu", "cpu_to_h264", "cpu_to_hevc",
               "gpu_to_h264", "gpu_to_hevc");
  CAPTURE(scenario);
  mw::streamer::InitConfig runtime_config;
  runtime_config.log.console_enabled = 0;
  runtime_config.event_poller_threads = 1;
  runtime_config.work_threads = 1;
  std::unique_ptr<mw::streamer::MwStreamerContext, ShutdownRuntime> runtime(
      mw::streamer::Init(runtime_config));
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  const auto* native_device = device.get()->data;
  ffmpeg::StreamInfo stream;
  const auto gpu_frames = ReadFrames(device, stream);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  ffmpeg::StreamInfo cpu_stream;
  const auto cpu_frames = ReadFrames(cpu, cpu_stream);
  std::vector<ffmpeg::Frame> gpu_references;
  std::vector<std::uint64_t> cpu_hashes;
  std::vector<std::uint64_t> hashes;
  for (const auto& frame : gpu_frames) {
    gpu_references.push_back(Download(frame));
    hashes.push_back(PixelHash(gpu_references.back()));
  }
  for (const auto& frame : cpu_frames) cpu_hashes.push_back(PixelHash(frame));
  const bool gpu_input = scenario.compare(0, 3, "gpu") == 0;
  const bool gpu_output = scenario != "cpu_to_cpu" && scenario != "gpu_to_cpu";
  const auto& input_frames = gpu_input ? gpu_frames : cpu_frames;
  std::vector<FrameSnapshot> snapshots;
  for (const auto& frame : input_frames) snapshots.push_back(Snapshot(frame));

  stream.time_base = kNanoseconds;
  stream.codec_parameters.get()->format =
      gpu_input ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  mw::streamer::EncoderConfig config;
  config.fps = {10, 1};
  config.video_bit_rate = 2000000;
  config.gop_size = 10;
  config.max_b_frames = 0;
  if (gpu_output) {
    config.video_encoder_name = scenario.find("h264") != std::string::npos
                                    ? "h264_nvenc"
                                    : "hevc_nvenc";
    config.video_options = {{"delay", "0"}, {"zerolatency", "1"}};
  } else {
    config.video_encoder_name = "libx264";
    config.video_options = {{"preset", "ultrafast"}, {"tune", "zerolatency"}};
  }

  Capture capture;
  mw::streamer::Encoder encoder;
  encoder.SetOnReady([&](const auto& streams) { capture.streams = streams; });
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet, std::int64_t dts_ns) {
    std::lock_guard lock(capture.mutex);
    capture.packets.push_back(packet.Ref());
    capture.dts_ns.push_back(dts_ns);
    capture.wake.notify_all();
  });
  encoder.SetOnEnded([&] {
    std::lock_guard lock(capture.mutex);
    ++capture.ended;
    capture.wake.notify_all();
  });
  encoder.SetOnError([&](int, std::string_view error) {
    std::lock_guard lock(capture.mutex);
    capture.error = error;
    capture.wake.notify_all();
  });
  // A shared input device does not force a software encoder onto the GPU.
  encoder.Start(config, {stream}, device);
  REQUIRE(capture.streams.size() == 1);
  for (std::size_t index = 0; index < input_frames.size(); ++index) {
    REQUIRE(encoder.SubmitVideo(input_frames[index]));
    std::unique_lock lock(capture.mutex);
    REQUIRE(capture.wake.wait_for(lock, 10s, [&] {
      return capture.packets.size() > index || !capture.error.empty();
    }));
    REQUIRE(capture.error.empty());
  }
  encoder.Drain();
  {
    std::unique_lock lock(capture.mutex);
    REQUIRE(capture.wake.wait_for(lock, 10s, [&] {
      return capture.ended != 0 || !capture.error.empty();
    }));
    REQUIRE(capture.error.empty());
    CHECK(capture.ended == 1);
  }
  encoder.Stop();
  CHECK(device.get()->data == native_device);
  REQUIRE(capture.packets.size() == input_frames.size());
  const auto& output_stream = capture.streams.front();
  CHECK(output_stream.codec_parameters.get()->width == 256);
  CHECK(output_stream.codec_parameters.get()->height == 144);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == output_stream.stream_index);
    CHECK(av_rescale_q(packet->pts, packet->time_base, kNanoseconds) ==
          static_cast<std::int64_t>(index) * kFrameDurationNs);
    CHECK(capture.dts_ns[index] ==
          kStartNs + static_cast<std::int64_t>(index) * kFrameDurationNs);
  }
  const auto decoded = DecodePackets(output_stream, capture.packets);
  REQUIRE(decoded.size() == input_frames.size());
  for (std::size_t index = 0; index < decoded.size(); ++index) {
    CAPTURE(index);
    CheckUnchanged(input_frames[index], snapshots[index]);
    CHECK(PixelHash(cpu_frames[index]) == cpu_hashes[index]);
    CHECK(PixelHash(Download(gpu_frames[index])) == hashes[index]);
    CHECK(av_rescale_q(decoded[index]->pts, decoded[index]->time_base,
                       kNanoseconds) ==
          static_cast<std::int64_t>(index) * kFrameDurationNs);
    const auto errors = PixelError(
        gpu_input ? gpu_references[index] : cpu_frames[index], decoded[index]);
    for (std::size_t plane = 0; plane < errors.size(); ++plane) {
      CAPTURE(plane, errors[plane]);
      CHECK(errors[plane] <= 25);
    }
  }
}

TEST_CASE("外层CUDA编码支持CBR和带峰值上限的VBR", "[.][encoder][cuda]") {
  const std::string name = GENERATE("h264_nvenc", "hevc_nvenc");
  const auto mode = GENERATE(mw::streamer::RateControl::kCbr,
                             mw::streamer::RateControl::kVbr);
  CAPTURE(name, mode);
  mw::streamer::InitConfig runtime_config;
  runtime_config.log.console_enabled = 0;
  runtime_config.event_poller_threads = 1;
  runtime_config.work_threads = 1;
  std::unique_ptr<mw::streamer::MwStreamerContext, ShutdownRuntime> runtime(
      mw::streamer::Init(runtime_config));
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  ffmpeg::StreamInfo stream;
  const auto frames = ReadFrames(device, stream);
  stream.time_base = kNanoseconds;
  stream.codec_parameters.get()->format = AV_PIX_FMT_NV12;
  mw::streamer::EncoderConfig config;
  config.video_encoder_name = name;
  config.fps = {10, 1};
  config.video_bit_rate = 2000000;
  config.rate_control = mode;
  config.max_bit_rate = mode == mw::streamer::RateControl::kVbr ? 3000000 : 0;
  config.gop_size = 10;
  config.max_b_frames = 0;
  config.video_options = {{"delay", "0"}, {"zerolatency", "1"}};
  Capture capture;
  mw::streamer::Encoder encoder;
  encoder.SetOnReady([&](const auto& streams) { capture.streams = streams; });
  encoder.SetOnPacket([&](const ffmpeg::Packet& packet, std::int64_t dts_ns) {
    std::lock_guard lock(capture.mutex);
    capture.packets.push_back(packet.Ref());
    capture.dts_ns.push_back(dts_ns);
  });
  encoder.SetOnEnded([&] {
    std::lock_guard lock(capture.mutex);
    ++capture.ended;
    capture.wake.notify_all();
  });
  encoder.SetOnError([&](int, std::string_view error) {
    std::lock_guard lock(capture.mutex);
    capture.error = error;
    capture.wake.notify_all();
  });
  encoder.Start(config, {stream}, device);
  REQUIRE(capture.streams.size() == 1);
  const auto& output = capture.streams.front();
  CHECK(output.codec_parameters.get()->bit_rate == config.video_bit_rate);
  CHECK(av_cmp_q(output.codec_parameters.get()->framerate, config.fps) == 0);
  constexpr std::size_t kFrames = 5;
  for (std::size_t index = 0; index < kFrames; ++index) {
    REQUIRE(encoder.SubmitVideo(frames[index]));
  }
  encoder.Drain();
  {
    std::unique_lock lock(capture.mutex);
    REQUIRE(capture.wake.wait_for(lock, 10s, [&] {
      return capture.ended != 0 || !capture.error.empty();
    }));
    REQUIRE(capture.error.empty());
    CHECK(capture.ended == 1);
  }
  encoder.Stop();
  REQUIRE(capture.packets.size() == kFrames);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->size > 0);
    CHECK(packet->stream_index == output.stream_index);
    CHECK(av_rescale_q(packet->pts, packet->time_base, kNanoseconds) ==
          static_cast<std::int64_t>(index) * kFrameDurationNs);
    CHECK(av_rescale_q(packet->dts, packet->time_base, kNanoseconds) ==
          static_cast<std::int64_t>(index) * kFrameDurationNs);
    CHECK(capture.dts_ns[index] ==
          kStartNs + static_cast<std::int64_t>(index) * kFrameDurationNs);
  }
  CHECK(DecodePackets(output, capture.packets).size() == kFrames);
}

}  // namespace

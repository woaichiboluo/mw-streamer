#include "mw/streamer/input/ffmpeg_frame_decoder.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "mw/streamer/ffmpeg/error.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::FfmpegFrameDecoder;
using namespace std::chrono_literals;

ffmpeg::StreamInfo AudioStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 0;
  stream.time_base = AVRational{1, 1000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
  parameters->format = AV_SAMPLE_FMT_S16;
  parameters->sample_rate = 16000;
  av_channel_layout_default(&parameters->ch_layout, 1);
  return stream;
}

ffmpeg::Packet AudioPacket(std::int64_t pts, int bytes = 320) {
  ffmpeg::Packet packet;
  ffmpeg::FfmpegException::throwIfError(av_new_packet(packet.get(), bytes),
                                        "分配测试 PCM Packet");
  std::memset(packet->data, 0x3a, static_cast<std::size_t>(bytes));
  packet->stream_index = 0;
  packet->pts = packet->dts = pts;
  packet->duration = 10;
  packet->time_base = AVRational{1, 1000};
  return packet;
}

ffmpeg::StreamInfo VideoStream(AVCodecID codec = AV_CODEC_ID_RAWVIDEO) {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 1;
  stream.time_base = AVRational{1, 90000};
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = codec;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->width = 16;
  parameters->height = 16;
  parameters->framerate = AVRational{30000, 1001};
  parameters->sample_aspect_ratio = AVRational{4, 3};
  return stream;
}

class InspectableDecoder final : public ffmpeg::Decoder {
 public:
  explicit InspectableDecoder(const ffmpeg::StreamInfo& stream)
      : Decoder(stream, AVMEDIA_TYPE_VIDEO, {}) {}

  const AVCodecContext& native() { return *context(); }
};

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

TEST_CASE("异步解码保留原始音频数据和时间戳并排空 EOF",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; }, [] {});
  REQUIRE_THROWS_AS(decoder.Push(ffmpeg::Packet()), std::invalid_argument);
  for (int index = 0; index < 20; ++index) {
    REQUIRE(decoder.Push(AudioPacket(index * 10)));
  }
  decoder.Finish();
  decoder.Finish();
  ffmpeg::Frame frame;
  std::vector<std::int64_t> timestamps;
  const bool finished = WaitFor([&] {
    while (decoder.TryReceive(frame)) {
      timestamps.push_back(frame->pts);
      REQUIRE(frame->pts == frame->best_effort_timestamp);
      REQUIRE(frame->duration == 10);
      REQUIRE(frame->time_base.num == 1);
      REQUIRE(frame->time_base.den == 1000);
      REQUIRE(frame->sample_rate == 16000);
      REQUIRE(frame->nb_samples == 160);
      REQUIRE(frame->format == AV_SAMPLE_FMT_S16);
      REQUIRE(frame->data[0][0] == 0x3a);
      REQUIRE(frame->data[0][319] == 0x3a);
    }
    return decoder.finished();
  });
  REQUIRE(finished);
  REQUIRE(timestamps.size() == 20);
  for (std::size_t index = 0; index < timestamps.size(); ++index) {
    REQUIRE(timestamps[index] == static_cast<std::int64_t>(index * 10));
  }
  REQUIRE(released == 20 * 320);
  REQUIRE(decoder.queued_bytes() == 0);
  REQUIRE_FALSE(decoder.wants_packet());
  REQUIRE(decoder.end_pts() == 200);
}

TEST_CASE("八帧输出队列外保留一帧待推且 Stop 释放所有保留包",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  std::atomic<int> decoded{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; }, [] {},
      [&] {
        ++decoded;
        return -1;
      });
  REQUIRE(WaitFor([&] { return decoder.blocked(); }));
  for (int index = 0; index < 40; ++index) {
    REQUIRE(decoder.Push(AudioPacket(index * 10)));
  }
  REQUIRE(WaitFor([&] {
    return decoder.blocked() && !decoder.wants_packet() &&
           released == (FfmpegFrameDecoder::kFrameCapacity + 1) * 320 &&
           decoded == FfmpegFrameDecoder::kFrameCapacity + 1;
  }));
  REQUIRE(decoder.queued_bytes() == (40 - 9) * 320);
  ffmpeg::Frame frame;
  REQUIRE(decoder.TryReceive(frame));
  REQUIRE(frame->pts == 0);
  REQUIRE(WaitFor([&] { return released == 10 * 320 && decoded == 10; }));
  const auto started = std::chrono::steady_clock::now();
  decoder.Stop();
  REQUIRE(std::chrono::steady_clock::now() - started < 1s);
  REQUIRE(released == 40 * 320);
  REQUIRE(decoder.queued_bytes() == 0);
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  REQUIRE_FALSE(decoder.TryReceive(frame));
  REQUIRE_FALSE(decoder.Push(AudioPacket(1000)));
  REQUIRE(released == 41 * 320);
  REQUIRE(decoder.queued_bytes() == 0);
}

TEST_CASE("输出队列满时 EOF 等待待推帧交付后完整排空",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  std::atomic<int> decoded{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; }, [] {},
      [&] {
        ++decoded;
        return -1;
      });
  for (int index = 0; index < 12; ++index) {
    REQUIRE(decoder.Push(AudioPacket(index * 10)));
  }
  decoder.Finish();
  REQUIRE(WaitFor([&] { return decoded == 9 && decoder.blocked(); }));
  REQUIRE_FALSE(decoder.finished());
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  int received = 0;
  ffmpeg::Frame frame;
  const bool finished = WaitFor([&] {
    while (decoder.TryReceive(frame)) {
      REQUIRE(frame->pts == received * 10);
      REQUIRE(frame->best_effort_timestamp == frame->pts);
      REQUIRE(frame->duration == 10);
      ++received;
    }
    return decoder.finished();
  });
  REQUIRE(finished);
  REQUIRE(received == 12);
  REQUIRE(decoded == 12);
  REQUIRE(released == 12 * 320);
  REQUIRE(decoder.queued_bytes() == 0);
  REQUIRE(decoder.end_pts() == 120);
}

TEST_CASE("异步解码 EOF 后重启保留既有帧引用并重新接收输入",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  std::atomic<int> notifications{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; }, [&] { ++notifications; });
  REQUIRE(decoder.Push(AudioPacket(10)));
  decoder.Finish();
  REQUIRE(WaitFor([&] { return notifications >= 2; }));
  REQUIRE_FALSE(decoder.finished());
  ffmpeg::Frame first;
  REQUIRE(decoder.TryReceive(first));
  REQUIRE(WaitFor([&] { return decoder.finished(); }));
  REQUIRE(decoder.end_pts() == 20);
  decoder.Restart();
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  REQUIRE(decoder.Push(AudioPacket(200)));
  decoder.Finish();
  ffmpeg::Frame second;
  REQUIRE(WaitFor([&] { return decoder.TryReceive(second); }));
  REQUIRE(second->pts == 200);
  REQUIRE(first->pts == 10);
  REQUIRE(first->data[0][319] == 0x3a);
  REQUIRE(WaitFor([&] { return decoder.finished(); }));
  REQUIRE(decoder.end_pts() == 210);
  REQUIRE(released == 640);
}

TEST_CASE("异步解码错误在有效帧排空后传播且不伪装 EOF",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  std::atomic<int> notifications{0};
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool resume = false;
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; },
      [&] {
        if (notifications.fetch_add(1) == 0) {
          std::unique_lock<std::mutex> lock(mutex);
          entered = true;
          changed.notify_all();
          changed.wait_for(lock, 3s, [&] { return resume; });
        }
      });
  REQUIRE(decoder.Push(AudioPacket(0)));
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, 3s, [&] { return entered; }));
  }
  REQUIRE(decoder.Push(AudioPacket(10, 1)));
  for (int index = 2; index < 10; ++index) {
    REQUIRE(decoder.Push(AudioPacket(index * 10)));
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    resume = true;
  }
  changed.notify_all();
  REQUIRE(
      WaitFor([&] { return released == 9 * 320 + 1 && notifications >= 3; }));
  REQUIRE(decoder.queued_bytes() == 0);
  REQUIRE_FALSE(decoder.finished());
  ffmpeg::Frame frame;
  REQUIRE(decoder.TryReceive(frame));
  REQUIRE(frame->pts == 0);
  REQUIRE(WaitFor([&] { return !decoder.wants_packet(); }));
  REQUIRE_THROWS_AS(decoder.TryReceive(frame), ffmpeg::FfmpegException);
  REQUIRE_FALSE(decoder.finished());
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  decoder.Stop();
  REQUIRE(decoder.queued_bytes() == 0);
}

TEST_CASE("异步解码回调异常不会逃出工作线程或停止边界",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) {
        released += bytes;
        throw std::runtime_error("release");
      },
      [] { throw std::runtime_error("notify"); });
  REQUIRE(decoder.Push(AudioPacket(0)));
  decoder.Finish();
  ffmpeg::Frame frame;
  REQUIRE(WaitFor([&] { return decoder.TryReceive(frame); }));
  REQUIRE(WaitFor([&] { return decoder.finished(); }));
  REQUIRE(released == 320);
  REQUIRE(decoder.queued_bytes() == 0);
  decoder.Stop();
}

TEST_CASE("源时间基 EOF 估计不改写真实帧 duration 或其他元信息",
          "[input][frame_decoder]") {
  ffmpeg::AudioDecoder decoder(AudioStream());
  ffmpeg::Frame frame;
  frame->pts = 144;
  frame->best_effort_timestamp = 144;
  frame->pkt_dts = 130;
  frame->time_base = AVRational{1, 1000};
  frame->duration = 0;
  frame->nb_samples = 160;
  frame->sample_rate = 16000;
  REQUIRE(decoder.FrameEndPts(frame) == 154);
  REQUIRE(frame->duration == 0);
  REQUIRE(frame->pts == 144);
  REQUIRE(frame->best_effort_timestamp == 144);
  REQUIRE(frame->pkt_dts == 130);
  REQUIRE(frame->time_base.num == 1);
  REQUIRE(frame->time_base.den == 1000);
  REQUIRE(frame->sample_rate == 16000);
  frame->duration = 3;
  REQUIRE(decoder.FrameEndPts(frame) == 147);
  REQUIRE(frame->duration == 3);
  frame->pts = AV_NOPTS_VALUE;
  REQUIRE(decoder.FrameEndPts(frame) == AV_NOPTS_VALUE);
  REQUIRE(frame->pts == AV_NOPTS_VALUE);
}

TEST_CASE("未知源 PTS 不会预测真实帧或虚构轨道结束时间",
          "[input][frame_decoder]") {
  std::atomic<std::size_t> released{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::AudioDecoder>(AudioStream()),
      [&](std::size_t bytes) { released += bytes; }, [] {});
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  REQUIRE(decoder.Push(AudioPacket(AV_NOPTS_VALUE)));
  decoder.Finish();
  ffmpeg::Frame frame;
  REQUIRE(WaitFor([&] { return decoder.TryReceive(frame); }));
  REQUIRE(frame->pts == AV_NOPTS_VALUE);
  REQUIRE(frame->best_effort_timestamp == AV_NOPTS_VALUE);
  REQUIRE(frame->pkt_dts == AV_NOPTS_VALUE);
  REQUIRE(frame->duration == 10);
  REQUIRE(frame->time_base.den == 1000);
  REQUIRE(WaitFor([&] { return decoder.finished(); }));
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
  REQUIRE(released == 320);
}

TEST_CASE("视频 EOF 使用源帧率和 parser 时长但不改写帧 duration",
          "[input][frame_decoder]") {
  ffmpeg::VideoDecoder decoder(VideoStream());
  ffmpeg::Frame frame;
  frame->pts = 90000;
  frame->duration = 0;
  REQUIRE(decoder.FrameEndPts(frame) == 93003);
  REQUIRE(decoder.FrameEndPts(frame, 1) == 96006);
  REQUIRE(frame->duration == 0);
  REQUIRE(frame->pts == 90000);
  frame->duration = 1000;
  REQUIRE(decoder.FrameEndPts(frame, 1) == 91000);

  InspectableDecoder inspected(VideoStream());
  REQUIRE(inspected.native().framerate.num == 30000);
  REQUIRE(inspected.native().framerate.den == 1001);
  REQUIRE(inspected.native().sample_aspect_ratio.num == 4);
  REQUIRE(inspected.native().sample_aspect_ratio.den == 3);
}

TEST_CASE("VP8 VP9 默认解码器优先选择可用的 libvpx", "[input][frame_decoder]") {
  for (const auto codec : {AV_CODEC_ID_VP8, AV_CODEC_ID_VP9}) {
    InspectableDecoder decoder(VideoStream(codec));
    const auto* expected = avcodec_find_decoder_by_name(
        codec == AV_CODEC_ID_VP8 ? "libvpx" : "libvpx-vp9");
    if (!expected) expected = avcodec_find_decoder(codec);
    REQUIRE(expected != nullptr);
    REQUIRE(decoder.native().codec == expected);
  }
}

TEST_CASE("异步视频 EOF 使用 parser 快照且真实帧保持解码器元信息",
          "[input][frame_decoder]") {
  ffmpeg::Packet packet;
  ffmpeg::FfmpegException::throwIfError(
      av_new_packet(packet.get(), 16 * 16 * 3 / 2), "分配测试原始视频 Packet");
  std::memset(packet->data, 0x7f, static_cast<std::size_t>(packet->size));
  packet->pts = packet->dts = 90000;
  packet->duration = 0;
  packet->stream_index = 1;
  ffmpeg::VideoDecoder native(VideoStream());
  REQUIRE(native.SendPacket(packet));
  ffmpeg::Frame expected;
  REQUIRE(native.ReceiveFrame(expected) == ffmpeg::DecodeResult::kFrame);

  std::atomic<int> parser_calls{0};
  FfmpegFrameDecoder decoder(
      std::make_unique<ffmpeg::VideoDecoder>(VideoStream()), [](std::size_t) {},
      [] {},
      [&] {
        ++parser_calls;
        return 1;
      });
  REQUIRE(decoder.Push(std::move(packet)));
  decoder.Finish();
  ffmpeg::Frame frame;
  REQUIRE(WaitFor([&] { return decoder.TryReceive(frame); }));
  REQUIRE(frame->pts == expected->best_effort_timestamp);
  REQUIRE(frame->duration == expected->duration);
  REQUIRE(frame->pkt_dts == expected->pkt_dts);
  REQUIRE(frame->time_base.num == expected->time_base.num);
  REQUIRE(frame->time_base.den == expected->time_base.den);
  REQUIRE(WaitFor([&] { return decoder.finished(); }));
  REQUIRE(parser_calls == 1);
  REQUIRE(decoder.end_pts() == native.FrameEndPts(frame, 1));
  decoder.Stop();
  REQUIRE(decoder.end_pts() == AV_NOPTS_VALUE);
}

}  // namespace

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <stdexcept>

#include "encoder_test_support.h"

namespace {

using namespace encoder_test;
using mw::streamer::Encoder;

ffmpeg::Frame Nv12VideoFrame(int index) {
  ffmpeg::Frame frame;
  frame->width = 64;
  frame->height = 64;
  frame->format = AV_PIX_FMT_NV12;
  frame->time_base = kVideoTimeBase;
  frame->pts = kVideoStart + index;
  frame->duration = 1;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配 NV12 视频测试帧");
  for (int plane = 0; plane < 2; ++plane) {
    const int rows = plane == 0 ? 64 : 32;
    const int value = plane == 0 ? 20 + index * 20 : 128;
    for (int row = 0; row < rows; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane], value, 64);
    }
  }
  return frame;
}

TEST_CASE("视频像素格式默认沿用轨道且显式配置优先",
          "[encoder][configuration][pixel-format]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  CHECK(config.video_pixel_format == AV_PIX_FMT_NONE);
  auto source = VideoStream();
  SECTION("未指定格式时使用 NV12 输入轨道") {
    source.codec_parameters.get()->format = AV_PIX_FMT_NV12;
  }
  SECTION("显式 NV12 格式覆盖 YUV420P 输入轨道") {
    config.video_pixel_format = AV_PIX_FMT_NV12;
  }
  const auto source_format = source.codec_parameters.get()->format;
  encoder.Start(config, {source}, cpu);
  CHECK(source.codec_parameters.get()->format == source_format);
  for (int index = 0; index < 3; ++index) {
    REQUIRE(encoder.SubmitFrame(Video(Nv12VideoFrame(index))));
    REQUIRE(capture.WaitPackets(static_cast<std::size_t>(index + 1)));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  CHECK(capture.ended == 1);
  CHECK(capture.callback_error.empty());
  CHECK(source.codec_parameters.get()->format == source_format);
  REQUIRE(capture.streams.size() == 1);
  const auto& encoded_stream = capture.streams.front();
  ffmpeg::VideoDecoder decoder(encoded_stream);
  const auto frames =
      Decode(decoder, capture.packets, encoded_stream.stream_index);
  REQUIRE(frames.size() == 3);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    CHECK(frames[index]->width == 64);
    CHECK(frames[index]->height == 64);
    const int expected = 20 + static_cast<int>(index) * 20;
    const int actual = frames[index]->data[0][0];
    CHECK(actual >= expected - 4);
    CHECK(actual <= expected + 4);
  }
}

TEST_CASE("无效视频像素格式启动失败后可重新启动",
          "[encoder][configuration][pixel-format]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  SECTION("不存在的像素格式") { config.video_pixel_format = AV_PIX_FMT_NB; }
  SECTION("编码器不支持的像素格式") {
    config.video_pixel_format = AV_PIX_FMT_RGB24;
  }
  CHECK_THROWS(encoder.Start(config, {VideoStream()}, cpu));
  CHECK(capture.ready == 0);
  CHECK(capture.errors == 0);
  REQUIRE_NOTHROW(encoder.Start(Config(), {VideoStream()}, cpu));
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.ready == 1);
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  REQUIRE(capture.streams.size() == 1);
  const auto& encoded_stream = capture.streams.front();
  ffmpeg::VideoDecoder decoder(encoded_stream);
  CHECK(Decode(decoder, capture.packets, encoded_stream.stream_index).size() ==
        1);
}

TEST_CASE("显式视频像素格式不会自动转换不匹配的输入帧",
          "[encoder][configuration][pixel-format]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  config.video_pixel_format = AV_PIX_FMT_NV12;
  encoder.Start(config, {VideoStream()}, cpu);
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 1);
  CHECK(capture.ended == 0);
  CHECK(capture.packets.empty());
  CHECK(capture.callback_error.empty());
}

TEST_CASE("编码器启动提供输出轨道并允许停止后重新启动",
          "[encoder][lifecycle]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  for (int session = 0; session < 2; ++session) {
    encoder.Start(Config(), {VideoStream()}, cpu);
    CHECK_THROWS(encoder.Start(Config(), {VideoStream()}, cpu));
    CHECK(capture.ready == session + 1);
    REQUIRE(capture.streams.size() == 1);
    const auto& stream = capture.streams.front();
    CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264);
    CHECK(stream.codec_parameters.get()->format == AV_PIX_FMT_YUV420P);
    CHECK(stream.codec_parameters.get()->width == 64);
    CHECK(stream.codec_parameters.get()->height == 64);
    CHECK(stream.time_base.num > 0);
    REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
    encoder.Drain();
    REQUIRE(capture.Wait());
    encoder.Stop();
    CHECK(capture.ended == 1);
    CHECK(capture.errors == 0);
    const auto count = capture.packets.size();
    CHECK_FALSE(encoder.SubmitFrame(Video(VideoFrame(1))));
    encoder.Drain();
    encoder.Stop();
    CHECK(capture.packets.size() == count);
    // Wait() observes completed sessions; reset only after all workers joined.
    capture.ended = 0;
  }
  CHECK(capture.callback_error.empty());
}

TEST_CASE("无媒体帧的编码会话只通知一次结束", "[encoder][lifecycle]") {
  for (const auto& streams : std::vector<std::vector<ffmpeg::StreamInfo>>{
           {}, {VideoStream()}, {AudioStream()}}) {
    for (bool drain_first : {false, true}) {
      INFO("tracks=" << streams.size() << " drain_first=" << drain_first);
      Capture capture;
      Encoder encoder;
      capture.Bind(encoder);
      ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
      encoder.Start(Config(), streams, cpu);
      if (drain_first) {
        encoder.Drain();
        REQUIRE(capture.Wait());
      }
      encoder.Stop();
      encoder.Stop();
      encoder.Drain();
      CHECK(capture.ready == 1);
      CHECK(capture.ended == 1);
      CHECK(capture.errors == 0);
      CHECK(capture.packets.empty());
    }
  }
}

TEST_CASE("初始化失败回滚且扩展选项不能覆盖正式字段",
          "[encoder][configuration]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  for (const auto* key : {"b", "g", "bf", "rc", "maxrate"}) {
    Encoder encoder;
    auto config = Config();
    config.video_options[key] = "1";
    CHECK_THROWS(encoder.Start(config, {VideoStream()}, cpu));
    encoder.Stop();
  }
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  auto invalid = Config();
  invalid.video_encoder_name = "mw_encoder_does_not_exist";
  CHECK_THROWS(encoder.Start(invalid, {VideoStream()}, cpu));
  CHECK(capture.ready == 0);
  CHECK(capture.errors == 0);
  auto valid = Config();
  valid.video_options["profile"] = "high";
  REQUIRE_NOTHROW(encoder.Start(valid, {VideoStream()}, cpu));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.ready == 1);
  CHECK(capture.ended == 1);
}

TEST_CASE("编码错误通知一次且不会当作自然结束", "[encoder][lifecycle]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto malformed = VideoFrame(0);
  malformed->width = 32;
  REQUIRE(encoder.SubmitFrame(Frame(malformed, AudioFrame(2048))));
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 1);
  CHECK(capture.ended == 0);
  CHECK_FALSE(encoder.SubmitFrame(Video(VideoFrame(1))));
}

TEST_CASE("Stop 等待 Packet 回调并排空已接收的 frame",
          "[encoder][lifecycle][stop][backpressure]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder, &gate);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream()}, cpu);
  ReleaseGate release_before_encoder_destruction{gate};
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  REQUIRE(gate.Wait());
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(1))));
  std::promise<void> stop_started;
  auto stop = std::async(std::launch::async, [&] {
    stop_started.set_value();
    encoder.Stop();
  });
  ReleaseGate release_before_future_destruction{gate};
  stop_started.get_future().wait();
  CHECK(stop.wait_for(std::chrono::milliseconds(50)) ==
        std::future_status::timeout);
  gate.Release();
  REQUIRE(stop.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  stop.get();
  REQUIRE(capture.packets.size() == 2);
  CHECK(capture.packets[0]->pts == 0);
  CHECK(capture.packets[1]->pts == 1);
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  CHECK_FALSE(encoder.SubmitFrame(Video(VideoFrame(2))));
  encoder.Stop();
  encoder.Drain();
  CHECK(capture.packets.size() == 2);
  CHECK(capture.ended == 1);
  ffmpeg::VideoDecoder decoder(capture.streams.front());
  CHECK(Decode(decoder, capture.packets, capture.streams.front().stream_index)
            .size() == 2);
}

TEST_CASE("双轨编码会话拒绝缺失半个 channel frame", "[encoder][lifecycle]") {
  for (bool only_video : {false, true}) {
    INFO("only_video=" << only_video);
    Capture capture;
    Encoder encoder;
    capture.Bind(encoder);
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
    if (only_video) {
      CHECK_THROWS_AS(encoder.SubmitFrame(Video(VideoFrame(0))),
                      std::invalid_argument);
    } else {
      CHECK_THROWS_AS(encoder.SubmitFrame(Audio(AudioFrame(2048))),
                      std::invalid_argument);
    }
    encoder.Drain();
    REQUIRE(capture.Wait());
    encoder.Stop();
    encoder.Drain();
    CHECK(capture.ready == 1);
    CHECK(capture.ended == 1);
    CHECK(capture.errors == 0);
    CHECK(capture.packets.empty());
  }
}

TEST_CASE("CBR合法配置可编码且不接受矛盾峰值码率", "[encoder][configuration]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  for (const auto control :
       {mw::streamer::RateControl::kCbr, mw::streamer::RateControl::kVbr}) {
    Encoder encoder;
    auto config = Config();
    config.rate_control = control;
    config.max_bit_rate = config.video_bit_rate - 1;
    CHECK_THROWS(encoder.Start(config, {VideoStream()}, cpu));
  }
  {
    Encoder encoder;
    auto config = Config();
    config.rate_control = mw::streamer::RateControl::kCbr;
    config.max_bit_rate = config.video_bit_rate + 1;
    CHECK_THROWS(encoder.Start(config, {VideoStream()}, cpu));
  }
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  auto config = Config();
  config.rate_control = mw::streamer::RateControl::kCbr;
  config.max_bit_rate = config.video_bit_rate;
  config.video_options.erase("crf");
  REQUIRE_NOTHROW(encoder.Start(config, {VideoStream()}, cpu));
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  REQUIRE(capture.packets.size() == 1);
  CHECK(capture.packets.front()->size > 0);
}

TEST_CASE("拼错的编码器扩展选项在初始化时明确失败",
          "[encoder][configuration]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  config.video_options["preset_typo"] = "ultrafast";
  CHECK_THROWS(encoder.Start(config, {VideoStream()}, cpu));
  encoder.Stop();
  CHECK(capture.ready == 0);
  CHECK(capture.ended == 0);
  CHECK(capture.errors == 0);
  CHECK(capture.packets.empty());
}

TEST_CASE("Stop 排空期间的 Packet 致命错误取消排队 frame",
          "[encoder][lifecycle][stop][error][backpressure]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  encoder.SetOnPacket([&](const auto& packet) {
    {
      std::lock_guard lock(capture.mutex);
      capture.packets.push_back(packet.Ref());
      capture.wake.notify_all();
    }
    gate.Enter();
    throw std::runtime_error("测试 Packet 消费失败");
  });
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream()}, cpu);
  ReleaseGate release_before_encoder_destruction{gate};
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  REQUIRE(gate.Wait());
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(1))));
  std::promise<void> stop_started;
  auto stop = std::async(std::launch::async, [&] {
    stop_started.set_value();
    encoder.Stop();
  });
  ReleaseGate release_before_future_destruction{gate};
  stop_started.get_future().wait();
  CHECK(stop.wait_for(std::chrono::milliseconds(50)) ==
        std::future_status::timeout);
  gate.Release();
  REQUIRE(stop.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  stop.get();
  REQUIRE(capture.packets.size() == 1);
  CHECK(capture.packets.front()->pts == 0);
  CHECK(capture.errors == 1);
  CHECK(capture.ended == 0);
  CHECK_FALSE(encoder.SubmitFrame(Video(VideoFrame(2))));
  encoder.Stop();
  encoder.Drain();
  CHECK(capture.errors == 1);
  CHECK(capture.ended == 0);
  CHECK(capture.packets.size() == 1);
}

TEST_CASE("Stop 等待 OnEnded 回调并拒绝结束期间的新 frame",
          "[encoder][lifecycle][stop][callback]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  encoder.SetOnEnded([&]() noexcept {
    gate.Enter();
    std::lock_guard lock(capture.mutex);
    ++capture.ended;
    capture.wake.notify_all();
  });
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream()}, cpu);
  ReleaseGate release_before_encoder_destruction{gate};
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  auto stop = std::async(std::launch::async, [&] { encoder.Stop(); });
  ReleaseGate release_before_future_destruction{gate};
  REQUIRE(gate.Wait());
  CHECK(stop.wait_for(std::chrono::milliseconds(50)) ==
        std::future_status::timeout);
  CHECK_FALSE(encoder.SubmitFrame(Video(VideoFrame(1))));
  gate.Release();
  REQUIRE(stop.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  stop.get();
  REQUIRE(capture.packets.size() == 1);
  CHECK(capture.packets.front()->pts == 0);
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  encoder.Stop();
  encoder.Drain();
  CHECK(capture.packets.size() == 1);
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
}

TEST_CASE("OnReady 异常执行启动回滚且允许停止后重新启动",
          "[encoder][lifecycle][stop][configuration][callback]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  int ready_attempts = 0;
  encoder.SetOnReady([&](const auto&) {
    ++ready_attempts;
    throw std::runtime_error("测试 Ready 初始化失败");
  });
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  CHECK_THROWS_AS(encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu),
                  std::runtime_error);
  CHECK(ready_attempts == 1);
  CHECK_FALSE(encoder.SubmitFrame(Frame(VideoFrame(0), AudioFrame(3008))));
  encoder.Stop();
  encoder.Drain();
  CHECK(capture.ready == 0);
  CHECK(capture.ended == 0);
  CHECK(capture.errors == 0);
  CHECK(capture.packets.empty());
  capture.Bind(encoder);
  REQUIRE_NOTHROW(encoder.Start(Config(), {VideoStream()}, cpu));
  REQUIRE(encoder.SubmitFrame(Video(VideoFrame(0))));
  encoder.Stop();
  CHECK(capture.ready == 1);
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  REQUIRE(capture.packets.size() == 1);
  CHECK(capture.packets.front()->pts == 0);
}

}  // namespace

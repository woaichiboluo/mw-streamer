#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>

#include "encoder_test_support.h"

namespace {

using namespace encoder_test;
using mw::streamer::Encoder;

const ffmpeg::StreamInfo& Stream(const Capture& capture, AVMediaType type) {
  for (const auto& stream : capture.streams) {
    if (stream.codec_parameters.get()->codec_type == type) return stream;
  }
  throw std::logic_error("缺少编码测试轨道");
}

std::vector<ffmpeg::Packet> Packets(const Capture& capture,
                                    const ffmpeg::StreamInfo& stream) {
  std::vector<ffmpeg::Packet> packets;
  for (const auto& packet : capture.packets) {
    if (packet->stream_index == stream.stream_index)
      packets.push_back(packet.Ref());
  }
  return packets;
}

TEST_CASE("实时 frame 缓存满时丢弃整份音视频且计数保留时间缺口",
          "[encoder][audio][video][timing][backpressure]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder, &gate);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  ReleaseGate release{gate};
  const auto first = Frame(VideoFrame(0), AudioFrame(1024));
  REQUIRE(encoder.SubmitFrame(first));
  REQUIRE(gate.Wait());
  // A current frame is processing and precisely one complete frame fits in
  // the realtime queue. Later submissions are valid, nonfatal overloads.
  for (int index = 1; index <= 5; ++index) {
    REQUIRE(encoder.SubmitFrame(Frame(
        VideoFrame(index), AudioFrame(1024, kAudioStart + index * 1024))));
  }
  gate.Release();
  REQUIRE(capture.WaitPackets(3));
  REQUIRE(encoder.SubmitFrame(
      Frame(VideoFrame(6), AudioFrame(1024, kAudioStart + 6 * 1024))));
  encoder.Drain();
  CHECK_FALSE(encoder.SubmitFrame(
      Frame(VideoFrame(7), AudioFrame(1024, kAudioStart + 7 * 1024))));
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  CHECK(capture.ended == 1);
  const auto& video_stream = Stream(capture, AVMEDIA_TYPE_VIDEO);
  const auto& audio_stream = Stream(capture, AVMEDIA_TYPE_AUDIO);
  const auto video_packets = Packets(capture, video_stream);
  const auto audio_packets = Packets(capture, audio_stream);
  REQUIRE(video_packets.size() == 3);
  constexpr std::int64_t kVideoPts[] = {0, 1, 6};
  for (std::size_t index = 0; index < video_packets.size(); ++index) {
    const auto& packet = video_packets[index];
    CHECK(av_cmp_q(packet->time_base, video_stream.time_base) == 0);
    CHECK(av_rescale_q(packet->pts, packet->time_base, kVideoTimeBase) ==
          kVideoPts[index]);
    CHECK(packet->dts == packet->pts);
  }
  REQUIRE(audio_packets.size() == 4);
  constexpr std::int64_t kAudioPts[] = {-1024, 0, 1024, 6144};
  for (std::size_t index = 0; index < audio_packets.size(); ++index) {
    CHECK(av_cmp_q(audio_packets[index]->time_base, kAudioTimeBase) == 0);
    CHECK(audio_packets[index]->pts == kAudioPts[index]);
    CHECK(audio_packets[index]->dts == kAudioPts[index]);
  }
  ffmpeg::VideoDecoder decoder(video_stream);
  const auto frames = Decode(decoder, video_packets, video_stream.stream_index);
  REQUIRE(frames.size() == 3);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const int expected = 20 + 20 * static_cast<int>(kVideoPts[index]);
    CHECK(std::abs(frames[index]->data[0][0] - expected) <= 2);
  }
  CHECK(first.video.value()->pts == kVideoStart);
  CHECK(first.audio.value()->pts == kAudioStart);
  CHECK(first.audio.value()->nb_samples == 1024);
}

TEST_CASE("纯音频使用样本计数并保留 AAC 负 DTS 编码延迟",
          "[encoder][audio][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {AudioStream()}, cpu);
  const auto input = AudioFrame(3072);
  REQUIRE(encoder.SubmitFrame(Audio(input)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  REQUIRE(capture.streams.size() == 1);
  const auto& stream = capture.streams.front();
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(stream.codec_parameters.get()->sample_rate == 48000);
  CHECK(av_cmp_q(stream.time_base, kAudioTimeBase) == 0);
  REQUIRE(capture.packets.size() == 4);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == stream.stream_index);
    CHECK(packet->pts == (static_cast<std::int64_t>(index) - 1) * 1024);
    CHECK(packet->dts == packet->pts);
    CHECK(av_cmp_q(packet->time_base, kAudioTimeBase) == 0);
  }
  CHECK(input->pts == kAudioStart);
  CHECK(input->duration == 3072);
  CHECK(input->nb_samples == 3072);
}

TEST_CASE("音频编码帧跨越过载缺口时保留原生滤镜 FIFO 时间戳",
          "[encoder][audio][video][timing][backpressure][filter]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder, &gate);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  ReleaseGate release{gate};
  REQUIRE(encoder.SubmitFrame(Frame(VideoFrame(0), AudioFrame(800))));
  REQUIRE(gate.Wait());
  for (int index = 1; index <= 5; ++index) {
    REQUIRE(encoder.SubmitFrame(
        Frame(VideoFrame(index), AudioFrame(800, kAudioStart + index * 800))));
  }
  gate.Release();
  REQUIRE(capture.WaitPackets(2));
  REQUIRE(encoder.SubmitFrame(
      Frame(VideoFrame(6), AudioFrame(800, kAudioStart + 4800))));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  const auto& audio_stream = Stream(capture, AVMEDIA_TYPE_AUDIO);
  const auto packets = Packets(capture, audio_stream);
  REQUIRE(packets.size() == 4);
  // The native buffersink emits 1024-sample blocks. The second block starts
  // in the buffered 576 samples at PTS 1024 and consumes 448 samples from the
  // later frame at PTS 4800. The short tail therefore begins at PTS 5248;
  // neither a custom FIFO clock nor inserted silence should compress it.
  constexpr std::int64_t kPts[] = {-1024, 0, 1024, 5248};
  for (std::size_t index = 0; index < packets.size(); ++index) {
    CHECK(packets[index]->pts == kPts[index]);
    CHECK(packets[index]->dts == kPts[index]);
  }
  CHECK(packets.back()->duration == 352);
  CHECK(Packets(capture, Stream(capture, AVMEDIA_TYPE_VIDEO)).size() == 3);
}

TEST_CASE("channel frame 不按输入音视频起点裁掉音频前缀",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto audio = AudioFrame(3008, kAudioStart - 960, 960);
  const auto video = VideoFrame(0);
  REQUIRE(encoder.SubmitFrame(Frame(video, audio)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  const auto& audio_stream = Stream(capture, AVMEDIA_TYPE_AUDIO);
  const auto& video_stream = Stream(capture, AVMEDIA_TYPE_VIDEO);
  const auto audio_packets = Packets(capture, audio_stream);
  const auto video_packets = Packets(capture, video_stream);
  REQUIRE(video_packets.size() == 1);
  CHECK(video_packets.front()->pts == 0);
  REQUIRE(audio_packets.size() == 4);
  std::int64_t encoded_samples = 0;
  for (std::size_t index = 0; index < audio_packets.size(); ++index) {
    CHECK(audio_packets[index]->pts ==
          (static_cast<std::int64_t>(index) - 1) * 1024);
    encoded_samples += audio_packets[index]->duration;
  }
  CHECK(encoded_samples -
            audio_stream.codec_parameters.get()->initial_padding ==
        3008);
  ffmpeg::AudioDecoder decoder(audio_stream);
  const auto frames = Decode(decoder, audio_packets, audio_stream.stream_index);
  bool checked_prefix = false;
  for (const auto& frame : frames) {
    if (frame->pts != 0) continue;
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    double mean = 0;
    for (int index = 256; index < 768; ++index) mean += samples[index];
    mean /= 512;
    CHECK(std::abs(mean - 0.75) < 0.1);
    checked_prefix = true;
  }
  CHECK(checked_prefix);
  CHECK(audio->pts == kAudioStart - 960);
  CHECK(audio->nb_samples == 3008);
  CHECK(video->pts == kVideoStart);
}

TEST_CASE("channel frame 的消费计数不依赖源帧媒体时钟",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto video = VideoFrame(0);
  auto audio = AudioFrame(2048);
  for (auto* frame : {video.get(), audio.get()}) {
    frame->pts = AV_NOPTS_VALUE;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->best_effort_timestamp = AV_NOPTS_VALUE;
    frame->time_base = {0, 1};
  }
  REQUIRE(encoder.SubmitFrame(Frame(video, audio)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  const auto videos = Packets(capture, Stream(capture, AVMEDIA_TYPE_VIDEO));
  const auto audios = Packets(capture, Stream(capture, AVMEDIA_TYPE_AUDIO));
  REQUIRE(videos.size() == 1);
  CHECK(videos.front()->pts == 0);
  REQUIRE(audios.size() == 3);
  CHECK(audios.front()->pts == -1024);
  CHECK(audios.back()->pts == 1024);
  CHECK(video->pts == AV_NOPTS_VALUE);
  CHECK(audio->pts == AV_NOPTS_VALUE);
  CHECK(video->time_base.num == 0);
  CHECK(audio->time_base.num == 0);
}

TEST_CASE("Stop 排空 B 帧保留原生负 DTS 与展示顺序",
          "[encoder][video][timing][stop]") {
  constexpr int kFrames = 12;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto config = Config();
  config.max_b_frames = 2;
  config.video_options["preset"] = "medium";
  config.video_options.erase("tune");
  config.video_options["x264-params"] =
      "b-adapt=0:rc-lookahead=5:sync-lookahead=0";
  encoder.Start(config, {VideoStream()}, cpu);
  // Feed small pictures at the real channel cadence so this codec-delay test
  // does not intentionally exercise the separate realtime overload policy.
  auto deadline = std::chrono::steady_clock::now();
  for (int index = 0; index < kFrames; ++index) {
    std::this_thread::sleep_until(deadline);
    REQUIRE(encoder.SubmitFrame(Video(VideoFrame(index))));
    deadline += std::chrono::milliseconds(20);
  }
  encoder.Stop();
  CHECK(capture.ended == 1);
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.packets.size() == kFrames);
  REQUIRE(capture.streams.size() == 1);
  CHECK(capture.packets.front()->dts < 0);
  std::vector<std::int64_t> presentation_times;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (index > 0) CHECK(packet->dts > capture.packets[index - 1]->dts);
    CHECK(av_cmp_q(packet->time_base, kVideoTimeBase) == 0);
    presentation_times.push_back(packet->pts);
  }
  std::sort(presentation_times.begin(), presentation_times.end());
  for (int index = 0; index < kFrames; ++index) {
    CHECK(presentation_times[index] == index);
  }
  const auto& stream = capture.streams.front();
  ffmpeg::VideoDecoder decoder(stream);
  CHECK(Decode(decoder, capture.packets, stream.stream_index).size() ==
        kFrames);
}

TEST_CASE("Stop 和析构排空 AAC 滤镜短尾及编码器延迟",
          "[encoder][audio][timing][lifecycle][stop]") {
  Capture capture;
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  auto encoder = std::make_unique<Encoder>();
  capture.Bind(*encoder);
  encoder->Start(Config(), {AudioStream()}, cpu);
  constexpr int kSamples = 3008;
  const auto input = AudioFrame(kSamples, kAudioStart, 960);
  REQUIRE(encoder->SubmitFrame(Audio(input)));
  SECTION("直接 Stop，无需先 Drain") {
    encoder->Stop();
    encoder->Stop();
    encoder->Drain();
    CHECK_FALSE(encoder->SubmitFrame(Audio(AudioFrame(1024))));
  }
  SECTION("析构前回调及其捕获对象保持存活") { encoder.reset(); }
  REQUIRE(capture.errors == 0);
  CHECK(capture.ended == 1);
  CHECK(capture.callback_error.empty());
  const auto& stream = Stream(capture, AVMEDIA_TYPE_AUDIO);
  REQUIRE(capture.packets.size() == 4);
  CHECK(stream.codec_parameters.get()->initial_padding == 1024);
  CHECK(capture.packets.front()->pts == -1024);
  CHECK(capture.packets.back()->pts == 2048);
  CHECK(capture.packets.back()->duration == kSamples - 2048);
  std::int64_t encoded_samples = 0;
  for (const auto& packet : capture.packets) {
    CHECK(av_cmp_q(packet->time_base, kAudioTimeBase) == 0);
    encoded_samples += packet->duration;
  }
  CHECK(encoded_samples - stream.codec_parameters.get()->initial_padding ==
        kSamples);
  ffmpeg::AudioDecoder decoder(stream);
  const auto decoded = Decode(decoder, capture.packets, stream.stream_index);
  std::int64_t decoded_samples = 0;
  bool checked_prefix = false;
  bool checked_tail = false;
  for (const auto& frame : decoded) {
    decoded_samples += frame->nb_samples;
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    if (frame->pts != 0 && frame->pts != 2048) continue;
    REQUIRE(frame->nb_samples >= 768);
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    double mean = 0;
    for (int index = 256; index < 768; ++index) mean += samples[index];
    mean /= 512;
    if (frame->pts == 0) {
      CHECK(std::abs(mean - 0.75) < 0.1);
      checked_prefix = true;
    } else {
      CHECK(std::abs(mean - 0.1) < 0.1);
      checked_tail = true;
    }
  }
  // Codec parameters carry the AAC priming delay into the native decoder.
  // It skips 1024 priming samples, but the 960-sample final packet still
  // decodes to a full 1024-sample block with 64 tail-padding samples.
  CHECK(decoded_samples == 3072);
  REQUIRE(decoded.size() == 3);
  CHECK(decoded.front()->pts == 0);
  CHECK(decoded.back()->pts == 2048);
  std::size_t skip_size = 0;
  CHECK(av_packet_get_side_data(capture.packets.front().get(),
                                AV_PKT_DATA_SKIP_SAMPLES,
                                &skip_size) == nullptr);
  CHECK(skip_size == 0);
  // Removing only the copied stream's priming metadata exposes that block;
  // the packet payload and native durations remain identical.
  auto without_padding = stream;
  without_padding.codec_parameters.get()->initial_padding = 0;
  ffmpeg::AudioDecoder uncompensated(without_padding);
  const auto untrimmed =
      Decode(uncompensated, capture.packets, stream.stream_index);
  std::int64_t untrimmed_samples = 0;
  for (const auto& frame : untrimmed) untrimmed_samples += frame->nb_samples;
  CHECK(untrimmed_samples == 4096);
  REQUIRE(untrimmed.size() == 4);
  CHECK(untrimmed.front()->pts == -1024);
  CHECK(stream.codec_parameters.get()->initial_padding == 1024);
  CHECK(checked_prefix);
  CHECK(checked_tail);
  CHECK(input->pts == kAudioStart);
  CHECK(input->nb_samples == kSamples);
  CHECK(input->duration == kSamples);
}

}  // namespace

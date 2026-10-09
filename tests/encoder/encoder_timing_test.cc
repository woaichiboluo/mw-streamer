#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
}

#include "encoder_test_support.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/input/ffmpeg_input.h"
#include "mw/streamer/scheduler/scheduler.h"

namespace {

using namespace encoder_test;
using mw::streamer::Encoder;

struct ClosePlaybackInput {
  void operator()(AVFormatContext* input) const {
    avformat_close_input(&input);
  }
};

struct ShutdownPlaybackRuntime {
  void operator()(mw::streamer::MwStreamerContext* context) const {
    mw::streamer::Shutdown(context);
  }
};

std::vector<ffmpeg::Frame> PlaybackReferenceAudio(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开倍速测试参考文件");
  std::unique_ptr<AVFormatContext, ClosePlaybackInput> input(raw);
  ffmpeg::FfmpegException::throwIfError(
      avformat_find_stream_info(input.get(), nullptr), "读取倍速测试参考轨道");
  const int index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  ffmpeg::FfmpegException::throwIfError(index, "查找倍速测试参考音频");
  const auto* source = input->streams[index];
  ffmpeg::StreamInfo stream{index, ffmpeg::CodecParameters(*source->codecpar),
                            source->time_base};
  std::vector<ffmpeg::Packet> packets;
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(result, "读取倍速测试参考包");
    if (packet->stream_index == index) packets.push_back(std::move(packet));
  }
  ffmpeg::AudioDecoder decoder(stream);
  return Decode(decoder, packets, index);
}

double PlaybackToneFrequency(const std::vector<ffmpeg::Frame>& frames) {
  std::vector<float> samples;
  for (const auto& frame : frames) {
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    const auto* begin = reinterpret_cast<const float*>(frame->extended_data[0]);
    samples.insert(samples.end(), begin, begin + frame->nb_samples);
  }
  REQUIRE(samples.size() > 100);
  // Ignore AAC priming and the padded tail when measuring the sine fixture.
  const auto begin = samples.size() / 10;
  const auto end = samples.size() - begin;
  int crossings = 0;
  for (auto index = begin + 1; index < end; ++index) {
    if (samples[index - 1] <= 0 && samples[index] > 0) ++crossings;
  }
  return static_cast<double>(crossings) * frames.front()->sample_rate /
         static_cast<double>(end - begin);
}

TEST_CASE("Input倍速经Scheduler和Encoder保持固定规格及变速变调内容",
          "[encoder][input][scheduler][playback-speed]") {
  mw::streamer::InitConfig runtime_config;
  runtime_config.log.console_enabled = 0;
  runtime_config.event_poller_threads = 1;
  runtime_config.work_threads = 1;
  std::unique_ptr<mw::streamer::MwStreamerContext, ShutdownPlaybackRuntime>
      runtime(mw::streamer::Init(runtime_config));
  const std::string path =
      std::string(MW_STREAMER_ENCODER_TEST_DATA_DIR) + "/h264_aac.mp4";
  const double original_frequency =
      PlaybackToneFrequency(PlaybackReferenceAudio(path));
  REQUIRE(original_frequency > 0);
  for (const double speed : {0.5, 2.0, 4.0, 8.0}) {
    INFO(fmt::format("playback_speed={}", speed));
    Capture capture;
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    Encoder encoder;
    capture.Bind(encoder);
    constexpr AVRational kOutputFrameRate{30, 1};
    const double output_frame_period = av_q2d(av_inv_q(kOutputFrameRate));
    mw::streamer::SchedulerConfig scheduler_config;
    scheduler_config.video_frame_rate = kOutputFrameRate;
    mw::streamer::Scheduler scheduler(scheduler_config);
    mw::streamer::FfmpegInputConfig input_config;
    input_config.auto_reconnect = false;
    input_config.playback_speed = speed;
    mw::streamer::FfmpegInput input(cpu, input_config);
    const auto run = [&](auto&& action) noexcept {
      try {
        action();
      } catch (const std::exception& error) {
        std::lock_guard lock(capture.mutex);
        ++capture.errors;
        capture.callback_error = error.what();
        capture.wake.notify_all();
      }
    };
    bool input_ended = false;
    bool valid_output_audio = true;
    bool accepted_frames = true;
    double source_frame_period = 0;
    AVRational source_video_time_base{0, 1};
    std::vector<std::pair<std::int64_t, std::int64_t>> selected_video_times;
    scheduler.SetOnVideo([&](const auto& frame) noexcept {
      run([&] {
        const auto media_time = av_rescale_q(
            frame->best_effort_timestamp, source_video_time_base, kNanoseconds);
        if (selected_video_times.empty() ||
            selected_video_times.back().first != media_time) {
          selected_video_times.emplace_back(media_time, frame->pts);
        }
        accepted_frames &= encoder.SubmitVideo(frame);
      });
    });
    scheduler.SetOnAudio([&](const auto& frame) noexcept {
      valid_output_audio &=
          frame->sample_rate == 48000 && frame->format == AV_SAMPLE_FMT_FLTP;
      run([&] {
        if (!encoder.SubmitAudio(frame)) {
          throw std::runtime_error("编码器拒绝倍速音频");
        }
      });
    });
    scheduler.SetOnEnded([&]() noexcept { encoder.Drain(); });
    input.SetOnReady([&](const auto& streams) noexcept {
      run([&] {
        auto output_streams = streams;
        for (auto& stream : output_streams) {
          auto* parameters = stream.codec_parameters.get();
          if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
            parameters->sample_rate = 48000;
            parameters->format = AV_SAMPLE_FMT_FLTP;
          } else if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (parameters->framerate.num <= 0 ||
                parameters->framerate.den <= 0) {
              throw std::runtime_error("倍速测试视频缺少原始帧率");
            }
            source_frame_period = av_q2d(av_inv_q(parameters->framerate));
            source_video_time_base = stream.time_base;
          }
        }
        auto config = Config();
        config.fps = kOutputFrameRate;
        encoder.Start(config, output_streams, cpu);
        if (!scheduler.Start(streams)) {
          throw std::runtime_error("Scheduler倍速会话启动失败");
        }
      });
    });
    input.SetOnFrame([&](int, const auto& frame) noexcept {
      run([&] {
        const bool accepted = frame->width > 0 ? scheduler.SubmitVideo(frame)
                                               : scheduler.SubmitAudio(frame);
        if (!accepted) throw std::runtime_error("Scheduler拒绝倍速输入");
      });
    });
    input.SetOnStateChanged([&](mw::streamer::InputState state, int,
                                std::string_view message) noexcept {
      run([&] {
        if (state == mw::streamer::InputState::kFailed) {
          throw std::runtime_error(std::string(message));
        }
        if (state == mw::streamer::InputState::kEnded) {
          input_ended = true;
          scheduler.Drain();
        }
      });
    });
    const auto started = std::chrono::steady_clock::now();
    input.Start(path);
    const bool ended = capture.Wait();
    input.Stop();
    scheduler.Stop();
    encoder.Stop();
    INFO(capture.callback_error);
    REQUIRE(ended);
    REQUIRE(capture.errors == 0);
    CHECK(capture.ended == 1);
    CHECK(input_ended);
    CHECK(accepted_frames);
    CHECK(valid_output_audio);
    const double expected_duration = 2.0 / speed;
    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    CHECK(elapsed >= expected_duration - output_frame_period);
    CHECK(elapsed <= expected_duration + 1.5);
    double video_end = 0;
    double audio_end = 0;
    for (const auto& stream : capture.streams) {
      const auto* parameters = stream.codec_parameters.get();
      if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
        CHECK(av_cmp_q(parameters->framerate, kOutputFrameRate) == 0);
        ffmpeg::VideoDecoder decoder(stream);
        const auto frames =
            Decode(decoder, capture.packets, stream.stream_index);
        REQUIRE_FALSE(frames.empty());
        for (std::size_t index = 0; index < frames.size(); ++index) {
          CHECK(av_rescale_q(frames[index]->pts, stream.time_base,
                             av_inv_q(kOutputFrameRate)) ==
                static_cast<std::int64_t>(index));
        }
        video_end = static_cast<double>(frames.size()) * output_frame_period;
      } else {
        CHECK(parameters->sample_rate == 48000);
        ffmpeg::AudioDecoder decoder(stream);
        const auto frames =
            Decode(decoder, capture.packets, stream.stream_index);
        REQUIRE_FALSE(frames.empty());
        const double frequency = PlaybackToneFrequency(frames);
        CHECK(std::abs(frequency / (original_frequency * speed) - 1) < 0.1);
        for (const auto& frame : frames) {
          audio_end = std::max(
              audio_end,
              static_cast<double>(frame->pts) * av_q2d(stream.time_base) +
                  static_cast<double>(frame->nb_samples) / 48000);
        }
      }
    }
    INFO(fmt::format(
        "expected={} video_end={} audio_end={} source_frame_period={}",
        expected_duration, video_end, audio_end, source_frame_period));
    REQUIRE(source_frame_period > 0);
    // An empty video queue freezes Scheduler's media clock. Sparse paced input
    // can therefore add one source frame period while the initial queue forms.
    const double max_duration = expected_duration +
                                source_frame_period / speed +
                                2 * output_frame_period;
    CHECK(video_end >= expected_duration - output_frame_period);
    CHECK(video_end <= max_duration);
    CHECK(audio_end >= expected_duration - output_frame_period);
    CHECK(audio_end <= max_duration);
    CHECK(std::abs(video_end - audio_end) < 0.1);
    REQUIRE(selected_video_times.size() > 2);
    // Exclude the first picture's queue startup delay, then verify the selected
    // content progresses at the requested speed rather than only counting
    // frames.
    const auto& first = selected_video_times[1];
    const auto& last = selected_video_times.back();
    const double source_span =
        static_cast<double>(last.first - first.first) / 1e9;
    const double output_span =
        static_cast<double>(last.second - first.second) / 1e9;
    INFO(
        fmt::format("source_span={} output_span={}", source_span, output_span));
    CHECK(std::abs(output_span - source_span / speed) <=
          2 * output_frame_period);
  }
}

TEST_CASE("满视频缓存重复最后画面并保留八个连续编码时刻",
          "[encoder][video][timing]") {
  Gate gate;
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder, &gate);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream()}, cpu);
  ReleaseGate release{gate};
  auto first = VideoFrame(0);
  REQUIRE(encoder.SubmitVideo(first));
  REQUIRE(gate.Wait());
  for (int picture = 1; picture < 8; ++picture) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(picture)));
  }
  encoder.Drain();
  CHECK_FALSE(encoder.SubmitVideo(VideoFrame(8)));
  gate.Release();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.packets.size() == 8);
  REQUIRE(capture.streams.size() == 1);
  const auto& stream = capture.streams.front();
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == stream.stream_index);
    CHECK(av_cmp_q(packet->time_base, stream.time_base) == 0);
    CHECK(av_rescale_q(packet->pts, packet->time_base, kNanoseconds) ==
          static_cast<std::int64_t>(index) * kFrameInterval);
    CHECK(capture.dts_ns[index] ==
          kStart + static_cast<std::int64_t>(index) * kFrameInterval);
  }
  CHECK(first->pts == kStart);
  CHECK(first->duration == kFrameInterval);
  ffmpeg::VideoDecoder decoder(stream);
  const auto frames = Decode(decoder, capture.packets, stream.stream_index);
  REQUIRE(frames.size() == 8);
  constexpr int kPictures[] = {0, 1, 2, 3, 4, 5, 5, 5};
  for (std::size_t index = 0; index < frames.size(); ++index) {
    REQUIRE(frames[index]->format == AV_PIX_FMT_YUV420P);
    for (int plane = 0; plane < 3; ++plane) {
      const int size = plane == 0 ? 64 : 32;
      const int expected = plane == 0 ? 20 + 20 * kPictures[index] : 128;
      int max_error = 0;
      for (int row = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column) {
          const int value =
              frames[index]
                  ->data[plane][row * frames[index]->linesize[plane] + column];
          max_error = std::max(max_error, std::abs(value - expected));
        }
      }
      CHECK(max_error <= 2);
    }
  }
}

TEST_CASE("纯音频按样本累计PTS并保留AAC编码延迟", "[encoder][audio][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {AudioStream()}, cpu);
  const auto input = AudioFrame(3072);
  REQUIRE(encoder.SubmitAudio(input));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.errors == 0);
  REQUIRE(capture.streams.size() == 1);
  const auto& stream = capture.streams.front();
  CHECK(stream.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(stream.codec_parameters.get()->sample_rate == 48000);
  CHECK(stream.codec_parameters.get()->format == AV_SAMPLE_FMT_FLTP);
  CHECK(stream.codec_parameters.get()->ch_layout.nb_channels == 2);
  CHECK(av_cmp_q(stream.time_base, {1, 48000}) == 0);
  REQUIRE(capture.packets.size() == 4);
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    CHECK(packet->stream_index == stream.stream_index);
    CHECK(packet->pts == (static_cast<std::int64_t>(index) - 1) * 1024);
    CHECK(packet->dts == packet->pts);
    CHECK(capture.dts_ns[index] ==
          kStart + av_rescale_q(packet->dts, stream.time_base, kNanoseconds));
  }
  CHECK(input->pts == kStart);
  CHECK(input->nb_samples == 3072);
}

TEST_CASE("音频等待视频起点并裁剪提前二十毫秒的样本",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto audio = AudioFrame(3008, kStart - kFrameInterval, 960);
  REQUIRE(encoder.SubmitAudio(audio));
  REQUIRE(encoder.SubmitVideo(VideoFrame(0)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.streams.size() == 2);
  const ffmpeg::StreamInfo* audio_stream = nullptr;
  const ffmpeg::StreamInfo* video_stream = nullptr;
  for (const auto& stream : capture.streams) {
    if (stream.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream = &stream;
    } else {
      video_stream = &stream;
    }
  }
  REQUIRE(audio_stream != nullptr);
  REQUIRE(video_stream != nullptr);
  int audio_packets = 0;
  int video_packets = 0;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (packet->stream_index == audio_stream->stream_index) {
      CHECK(packet->pts == (audio_packets - 1) * 1024);
      CHECK(capture.dts_ns[index] ==
            kStart +
                av_rescale_q(packet->dts, packet->time_base, kNanoseconds));
      ++audio_packets;
    } else {
      CHECK(packet->stream_index == video_stream->stream_index);
      CHECK(packet->pts == 0);
      CHECK(capture.dts_ns[index] == kStart);
      ++video_packets;
    }
  }
  CHECK(audio_packets == 3);
  CHECK(video_packets == 1);
  ffmpeg::AudioDecoder decoder(*audio_stream);
  const auto frames =
      Decode(decoder, capture.packets, audio_stream->stream_index);
  bool checked_samples = false;
  for (const auto& frame : frames) {
    if (frame->pts != 0) continue;
    REQUIRE(frame->format == AV_SAMPLE_FMT_FLTP);
    const auto* samples =
        reinterpret_cast<const float*>(frame->extended_data[0]);
    double mean = 0;
    for (int index = 256; index < frame->nb_samples; ++index)
      mean += static_cast<double>(samples[index]);
    mean /= frame->nb_samples - 256;
    CHECK(std::abs(mean - 0.1) < 0.05);
    checked_samples = true;
  }
  CHECK(checked_samples);
  CHECK(audio->pts == kStart - kFrameInterval);
  CHECK(audio->nb_samples == 3008);
}

TEST_CASE("零时间戳可以作为音视频会话的有效同步起点",
          "[encoder][audio][video][timing]") {
  Capture capture;
  Encoder encoder;
  capture.Bind(encoder);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  encoder.Start(Config(), {VideoStream(), AudioStream()}, cpu);
  auto video = VideoFrame(0);
  video->pts = 0;
  REQUIRE(encoder.SubmitVideo(video));
  REQUIRE(encoder.SubmitAudio(AudioFrame(2048, 0)));
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  CHECK(capture.ended == 1);
  CHECK(capture.errors == 0);
  int video_packets = 0;
  int audio_packets = 0;
  const int video_index = VideoStream().stream_index;
  const int audio_index = AudioStream().stream_index;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (packet->stream_index == video_index) {
      CHECK(packet->pts == 0);
      CHECK(capture.dts_ns[index] == 0);
      ++video_packets;
    } else {
      CHECK(packet->stream_index == audio_index);
      CHECK(packet->pts == (audio_packets - 1) * 1024);
      CHECK(capture.dts_ns[index] ==
            av_rescale_q(packet->dts, packet->time_base, kNanoseconds));
      ++audio_packets;
    }
  }
  CHECK(video_packets == 1);
  CHECK(audio_packets == 3);
}

TEST_CASE("B帧延迟编码排空并保持原生DTS与同步域映射",
          "[encoder][video][timing]") {
  constexpr int kFrames = 40;
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
  for (int index = 0; index < kFrames; ++index) {
    REQUIRE(encoder.SubmitVideo(VideoFrame(index)));
  }
  encoder.Drain();
  REQUIRE(capture.Wait());
  encoder.Stop();
  REQUIRE(capture.errors == 0);
  REQUIRE(capture.packets.size() == kFrames);
  REQUIRE(capture.streams.size() == 1);
  const auto first_dts = capture.packets.front()->dts;
  CHECK(first_dts < 0);
  CHECK(capture.dts_ns.front() == kStart);
  std::vector<std::int64_t> presentation_times;
  for (std::size_t index = 0; index < capture.packets.size(); ++index) {
    const auto& packet = capture.packets[index];
    if (index > 0) CHECK(packet->dts > capture.packets[index - 1]->dts);
    CHECK(capture.dts_ns[index] ==
          kStart + av_rescale_q(packet->dts - first_dts, packet->time_base,
                                kNanoseconds));
    presentation_times.push_back(
        av_rescale_q(packet->pts, packet->time_base, {1, 50}));
  }
  std::sort(presentation_times.begin(), presentation_times.end());
  for (std::size_t index = 0; index < presentation_times.size(); ++index) {
    CHECK(presentation_times[index] == static_cast<std::int64_t>(index));
  }
  const auto& stream = capture.streams.front();
  ffmpeg::VideoDecoder decoder(stream);
  const auto frames = Decode(decoder, capture.packets, stream.stream_index);
  CHECK(frames.size() == kFrames);
}

}  // namespace

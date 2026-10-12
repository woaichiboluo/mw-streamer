#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/input/ffmpeg_input.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

namespace {

using namespace std::chrono_literals;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputMode;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::DecodedInputSource;

std::string SourceSample(const char* name) {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + name;
}

struct ReferenceSource {
  std::int64_t start_time_us = 0;
  std::int64_t duration_us = AV_NOPTS_VALUE;
  std::size_t packets = 0;
  std::map<int, AVRational> time_bases;
  std::map<int, std::vector<ffmpeg::Frame>> frames;
};

ReferenceSource DecodeSourceReference(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "打开按轨读取参考源");
  const auto close = [](AVFormatContext* value) {
    avformat_close_input(&value);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "读取按轨参考信息");
  ReferenceSource reference;
  reference.start_time_us =
      raw->start_time == AV_NOPTS_VALUE ? 0 : raw->start_time;
  reference.duration_us = raw->duration;
  std::map<int, ffmpeg::CodecContext> decoders;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int index = av_find_best_stream(raw, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) continue;
    ffmpeg::FfmpegException::throwIfError(index, "选择按轨参考媒体");
    const auto* stream = raw->streams[index];
    ffmpeg::CodecContext decoder(
        avcodec_find_decoder(stream->codecpar->codec_id));
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(decoder.get(), stream->codecpar),
        "设置按轨参考解码器");
    decoder.get()->pkt_timebase = stream->time_base;
    decoder.get()->thread_count = 0;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr), "打开按轨参考解码器");
    decoders.emplace(index, std::move(decoder));
    reference.time_bases.emplace(index, stream->time_base);
  }
  const auto receive = [&](int index, ffmpeg::CodecContext& decoder) {
    ffmpeg::Frame frame;
    for (;;) {
      const int status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
      ffmpeg::FfmpegException::throwIfError(status, "接收按轨参考帧");
      reference.frames[index].push_back(frame.Ref());
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "读取按轨参考包");
    const auto found = decoders.find(packet->stream_index);
    if (found == decoders.end()) continue;
    ++reference.packets;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(found->second.get(), packet.get()),
        "提交按轨参考包");
    receive(found->first, found->second);
  }
  for (auto& [index, decoder] : decoders) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder.get(), nullptr), "排空按轨参考解码器");
    receive(index, decoder);
  }
  return reference;
}

struct PullCapture {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<DecodedInputSource> sources;
  std::vector<InputState> states;
  std::size_t packets = 0;
  int track_ends = 0;

  void Attach(FfmpegInput& input) {
    input.SetOnDecodedReady([this](const auto& source) {
      std::lock_guard<std::mutex> lock(mutex);
      sources.push_back(source);
      changed.notify_all();
    });
    input.SetOnPacket([this](std::uint64_t, const auto&) {
      std::lock_guard<std::mutex> lock(mutex);
      ++packets;
      changed.notify_all();
    });
    input.SetOnTrackEnded([this](int, std::int64_t) {
      std::lock_guard<std::mutex> lock(mutex);
      ++track_ends;
      changed.notify_all();
    });
    input.SetOnStateChanged([this](InputState state, int, auto) {
      std::lock_guard<std::mutex> lock(mutex);
      states.push_back(state);
      changed.notify_all();
    });
  }
  bool WaitSources(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 8s, [&] { return sources.size() >= count; });
  }
  bool WaitState(InputState state) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, 8s, [&] {
      return std::find(states.begin(), states.end(), state) != states.end();
    });
  }
  DecodedInputSource Source(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    return sources.at(index);
  }
};

bool ReceiveBefore(const mw::streamer::InputFrameSource& track,
                   ffmpeg::Frame& frame,
                   std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    if (track.try_receive(frame)) return true;
    if (track.finished()) return false;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

std::int64_t PulledFrameCount(
    const DecodedInputSource& source,
    const std::map<int, std::vector<ffmpeg::Frame>>& frames) {
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    for (const auto& track : source.tracks) {
      if (track.stream.codec_parameters.get()->codec_type == type) {
        return static_cast<std::int64_t>(
            frames.at(track.stream.stream_index).size());
      }
    }
  }
  return 0;
}

void CheckRawPullFrame(const ffmpeg::Frame& actual,
                       const ffmpeg::Frame& expected, AVRational time_base) {
  CHECK(actual->pts == expected->best_effort_timestamp);
  CHECK(actual->best_effort_timestamp == expected->best_effort_timestamp);
  CHECK(actual->pkt_dts == expected->pkt_dts);
  CHECK(actual->duration == expected->duration);
  CHECK(av_cmp_q(actual->time_base, time_base) == 0);
  REQUIRE(actual->format == expected->format);
  CHECK(actual->width == expected->width);
  CHECK(actual->height == expected->height);
  CHECK(actual->sample_rate == expected->sample_rate);
  REQUIRE(actual->nb_samples == expected->nb_samples);
  if (expected->sample_rate <= 0) {
    REQUIRE(actual->buf[0]);
    CHECK(std::memcmp(actual->data[0], expected->data[0],
                      static_cast<std::size_t>(expected->width)) == 0);
    return;
  }
  REQUIRE(av_channel_layout_compare(&actual->ch_layout, &expected->ch_layout) ==
          0);
  const auto format = static_cast<AVSampleFormat>(expected->format);
  const bool planar = av_sample_fmt_is_planar(format) != 0;
  const int planes = planar ? expected->ch_layout.nb_channels : 1;
  const int bytes = expected->nb_samples * av_get_bytes_per_sample(format) *
                    (planar ? 1 : expected->ch_layout.nb_channels);
  REQUIRE(bytes > 0);
  for (int plane = 0; plane < planes; ++plane) {
    CHECK(std::memcmp(actual->extended_data[plane],
                      expected->extended_data[plane],
                      static_cast<std::size_t>(bytes)) == 0);
  }
}

TEST_CASE("Input 每轨原始 EOF 边界在最后一帧之后且全局 EOF 之前交付",
          "[input][source][eof]") {
  FfmpegInputConfig config;
  config.mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ffmpeg::StreamInfo> streams;
  std::map<int, ffmpeg::Frame> last;
  std::map<int, std::int64_t> ends;
  std::map<int, int> end_counts;
  bool terminal = false;
  bool failed = false;
  bool valid = true;
  FfmpegInput input(config);
  input.SetOnReady([&](const auto& descriptions) { streams = descriptions; });
  input.SetOnFrame([&](int index, const auto& frame) {
    valid &= ends.count(index) == 0;
    valid &= frame->pts == frame->best_effort_timestamp;
    last.insert_or_assign(index, frame.Ref());
  });
  input.SetOnTrackEnded([&](int index, std::int64_t end_pts) {
    valid &= last.count(index) == 1;
    ends.insert_or_assign(index, end_pts);
    ++end_counts[index];
  });
  input.SetOnStateChanged([&](auto state, int, auto) {
    if (state != InputState::kEnded && state != InputState::kFailed) return;
    std::lock_guard<std::mutex> lock(mutex);
    failed = state == InputState::kFailed;
    valid &= ends.size() == streams.size();
    terminal = true;
    changed.notify_all();
  });
  input.Start(std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/h265_aac.mp4");
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, 8s, [&] { return terminal; }));
  }
  input.Stop();
  REQUIRE_FALSE(failed);
  REQUIRE(valid);
  REQUIRE(streams.size() == 2);
  for (const auto& stream : streams) {
    const auto& frame = last.at(stream.stream_index);
    REQUIRE(frame->duration > 0);
    CHECK(av_cmp_q(frame->time_base, stream.time_base) == 0);
    CHECK(ends.at(stream.stream_index) == frame->pts + frame->duration);
    CHECK(end_counts.at(stream.stream_index) == 1);
  }
}

TEST_CASE("Input 的显式缓存失败不伪装成轨道 EOF", "[input][source][error]") {
  FfmpegInputConfig config;
  config.max_packet_buffer_bytes = 1;
  std::mutex mutex;
  std::condition_variable changed;
  bool failed = false;
  int eof_tracks = 0;
  FfmpegInput input(config);
  input.SetOnTrackEnded([&](int, std::int64_t) { ++eof_tracks; });
  input.SetOnStateChanged([&](auto state, int, auto) {
    if (state != InputState::kFailed) return;
    std::lock_guard<std::mutex> lock(mutex);
    failed = true;
    changed.notify_all();
  });
  input.Start(std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/h264_aac.mp4");
  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(changed.wait_for(lock, 8s, [&] { return failed; }));
  }
  input.Stop();
  CHECK(eof_tracks == 0);
}

TEST_CASE("Input 自动帧交付和按轨读取接口互斥", "[input][source][pull]") {
  FfmpegInput input;
  SECTION("先配置自动交付") {
    input.SetOnFrame([](int, const auto&) {});
    CHECK_THROWS_AS(input.SetOnDecodedReady([](const auto&) {}),
                    std::logic_error);
    input.SetOnFrame({});
    CHECK_NOTHROW(input.SetOnDecodedReady([](const auto&) {}));
  }
  SECTION("先配置按轨读取") {
    input.SetOnDecodedReady([](const auto&) {});
    CHECK_THROWS_AS(input.SetOnFrame([](int, const auto&) {}),
                    std::logic_error);
    input.SetOnDecodedReady({});
    CHECK_NOTHROW(input.SetOnFrame([](int, const auto&) {}));
  }
}

TEST_CASE("Input 按轨读取完整原始帧且保留销毁后的 EOF 边界",
          "[input][source][pull][eof]") {
  const auto reference = DecodeSourceReference(SourceSample("h265_aac.mp4"));
  PullCapture capture;
  DecodedInputSource source;
  std::map<int, std::vector<ffmpeg::Frame>> received;
  {
    FfmpegInputConfig config;
    config.mode = GENERATE(InputMode::kLive, InputMode::kBatch);
    config.max_packet_buffer_bytes = 64 * 1024;
    FfmpegInput input(config);
    capture.Attach(input);
    input.Start(SourceSample("h265_aac.mp4"));
    REQUIRE(capture.WaitSources(1));
    source = capture.Source(0);
    REQUIRE(source.tracks.size() == reference.frames.size());
    CHECK(source.start_time_us == reference.start_time_us);
    CHECK(source.media_origin_us == reference.start_time_us);
    CHECK_FALSE(source.loop);
    CHECK_FALSE(source.flush);
    CHECK(source.duration_us == reference.duration_us);
    REQUIRE(source.complete);
    CHECK(input.state() == InputState::kConnecting);
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    for (;;) {
      bool finished = true;
      bool progress = false;
      for (const auto& track : source.tracks) {
        ffmpeg::Frame frame;
        if (track.try_receive(frame)) {
          received[track.stream.stream_index].push_back(frame.Ref());
          progress = true;
        }
        finished &= track.finished();
      }
      if (finished) break;
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      if (!progress) std::this_thread::sleep_for(1ms);
    }
    REQUIRE(capture.WaitState(InputState::kConnected));
    const auto produced = PulledFrameCount(source, received);
    source.complete(produced);
    source.complete(produced);
    REQUIRE(capture.WaitState(InputState::kEnded));
    input.Stop();
  }
  CHECK(capture.track_ends == static_cast<int>(source.tracks.size()));
  for (const auto& track : source.tracks) {
    const int index = track.stream.stream_index;
    const auto& expected = reference.frames.at(index);
    REQUIRE(received.at(index).size() == expected.size());
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
      CheckRawPullFrame(received.at(index)[frame], expected[frame],
                        reference.time_bases.at(index));
    }
    REQUIRE(expected.back()->duration > 0);
    CHECK(track.finished());
    CHECK(track.end_pts() ==
          expected.back()->best_effort_timestamp + expected.back()->duration);
    ffmpeg::Frame empty;
    CHECK_FALSE(track.try_receive(empty));
  }
  // Completing or releasing handles after destruction cannot access Input.
  CHECK_NOTHROW(source.complete(0));
  source.tracks.clear();
  capture.sources.clear();
}

TEST_CASE("Input 按轨消费者暂停形成背压且 Stop 不伪装 EOF",
          "[input][source][pull][backpressure]") {
  const auto reference =
      DecodeSourceReference(SourceSample("seek_h264_aac.mp4"));
  PullCapture capture;
  DecodedInputSource source;
  {
    FfmpegInput input;
    capture.Attach(input);
    input.Start(SourceSample("seek_h264_aac.mp4"));
    REQUIRE(capture.WaitSources(1));
    source = capture.Source(0);
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    for (const auto& track : source.tracks) {
      ffmpeg::Frame frame;
      REQUIRE(ReceiveBefore(track, frame, deadline));
      CHECK(frame->pts == frame->best_effort_timestamp);
    }
    REQUIRE(capture.WaitState(InputState::kConnected));
    // Stop consuming. A bounded output queue must eventually halt packet
    // dispatch despite a local source that has more media available.
    bool stalled = false;
    while (std::chrono::steady_clock::now() < deadline) {
      std::unique_lock<std::mutex> lock(capture.mutex);
      const auto count = capture.packets;
      if (!capture.changed.wait_for(lock, 50ms,
                                    [&] { return capture.packets != count; })) {
        stalled = count > 0 && count < reference.packets;
        if (stalled) break;
      }
    }
    REQUIRE(stalled);
    for (const auto& track : source.tracks) CHECK_FALSE(track.finished());
    CHECK(input.state() == InputState::kConnected);
    input.Stop();
  }
  CHECK(capture.track_ends == 0);
  CHECK(std::find(capture.states.begin(), capture.states.end(),
                  InputState::kEnded) == capture.states.end());
  CHECK(std::find(capture.states.begin(), capture.states.end(),
                  InputState::kFailed) == capture.states.end());
  for (const auto& track : source.tracks) {
    CHECK(track.finished());
    CHECK(track.end_pts() == AV_NOPTS_VALUE);
    ffmpeg::Frame empty;
    CHECK_FALSE(track.try_receive(empty));
  }
  source.tracks.clear();
  capture.sources.clear();
}

TEST_CASE("单帧按轨源循环完成后结束且保留销毁后的 EOF",
          "[input][source][pull][loop]") {
  const auto reference =
      DecodeSourceReference(SourceSample("seek_tail_h264.mp4"));
  REQUIRE(reference.frames.size() == 1);
  REQUIRE(reference.frames.begin()->second.size() == 1);
  PullCapture capture;
  DecodedInputSource source;
  ffmpeg::Frame retained;
  {
    FfmpegInputConfig config;
    config.loop = true;
    FfmpegInput input(config);
    capture.Attach(input);
    input.Start(SourceSample("seek_tail_h264.mp4"));
    REQUIRE(capture.WaitSources(1));
    source = capture.Source(0);
    REQUIRE(source.tracks.size() == 1);
    REQUIRE(source.complete);
    REQUIRE(ReceiveBefore(source.tracks.front(), retained,
                          std::chrono::steady_clock::now() + 8s));
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!source.tracks.front().finished()) {
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
    source.complete(1);
    source.complete(1);
    REQUIRE(capture.WaitState(InputState::kEnded));
    input.Stop();
  }
  REQUIRE(capture.sources.size() == 1);
  const auto& track = source.tracks.front();
  const auto& expected = reference.frames.at(track.stream.stream_index).front();
  CheckRawPullFrame(retained, expected,
                    reference.time_bases.at(track.stream.stream_index));
  CHECK(track.finished());
  CHECK(track.end_pts() ==
        expected->best_effort_timestamp + expected->duration);
  CHECK_NOTHROW(source.complete(1));
}

TEST_CASE("按轨源解码排空后等待消费者完成才循环且重复完成幂等",
          "[input][source][pull][loop][completion]") {
  const auto reference = DecodeSourceReference(SourceSample("h264_aac.mp4"));
  PullCapture capture;
  FfmpegInputConfig config;
  config.loop = true;
  FfmpegInput input(config);
  capture.Attach(input);
  input.Start(SourceSample("h264_aac.mp4"));
  REQUIRE(capture.WaitSources(1));
  auto first = capture.Source(0);
  REQUIRE(first.complete);
  CHECK(first.duration_us == reference.duration_us);
  CHECK(first.media_origin_us == reference.start_time_us);
  CHECK(first.loop);
  CHECK_FALSE(first.flush);
  std::map<int, std::vector<ffmpeg::Frame>> received;
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  for (;;) {
    bool finished = true;
    bool progress = false;
    for (const auto& track : first.tracks) {
      ffmpeg::Frame frame;
      if (track.try_receive(frame)) {
        received[track.stream.stream_index].push_back(frame.Ref());
        progress = true;
      }
      finished &= track.finished();
    }
    if (finished) break;
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    if (!progress) std::this_thread::sleep_for(1ms);
  }
  const auto produced = PulledFrameCount(first, received);
  REQUIRE(produced > 2);
  // Empty decoder queues are not filter EOF. The consumer may still have
  // buffered output; Input must neither loop nor report global EOF yet.
  {
    std::unique_lock<std::mutex> lock(capture.mutex);
    CHECK_FALSE(capture.changed.wait_for(lock, 50ms, [&] {
      return capture.sources.size() > 1 ||
             std::find(capture.states.begin(), capture.states.end(),
                       InputState::kEnded) != capture.states.end() ||
             std::find(capture.states.begin(), capture.states.end(),
                       InputState::kFailed) != capture.states.end();
    }));
  }
  first.complete(produced);
  REQUIRE(capture.WaitSources(2));
  auto second = capture.Source(1);
  CHECK(first.start_time_us == second.start_time_us);
  CHECK(first.duration_us == second.duration_us);
  CHECK(second.media_origin_us == first.media_origin_us);
  CHECK(second.loop);
  CHECK_FALSE(second.flush);
  first.complete(produced);
  {
    std::unique_lock<std::mutex> lock(capture.mutex);
    CHECK_FALSE(capture.changed.wait_for(
        lock, 50ms, [&] { return capture.sources.size() > 2; }));
  }
  for (const auto& track : first.tracks) {
    const auto& expected = reference.frames.at(track.stream.stream_index);
    const auto& actual = received.at(track.stream.stream_index);
    REQUIRE(actual.size() == expected.size());
    CheckRawPullFrame(actual.front(), expected.front(),
                      reference.time_bases.at(track.stream.stream_index));
    CHECK(track.finished());
    CHECK(track.end_pts() ==
          expected.back()->best_effort_timestamp + expected.back()->duration);
  }
  // Stop must also interrupt a new segment whose consumer never completes.
  input.Stop();
  CHECK_NOTHROW(second.complete(0));
}

TEST_CASE("按轨 Seek 保留容器原点和原始 PTS 且毫秒位置不先量化成输出帧",
          "[input][source][pull][seek]") {
  const auto reference =
      DecodeSourceReference(SourceSample("seek_h264_aac.mp4"));
  PullCapture capture;
  FfmpegInput input;
  capture.Attach(input);
  input.Start(SourceSample("seek_h264_aac.mp4"));
  REQUIRE(capture.WaitSources(1));
  auto initial = capture.Source(0);
  CHECK(initial.media_origin_us == reference.start_time_us);
  CHECK_FALSE(initial.flush);
  const auto video_track = [](const DecodedInputSource& source)
      -> const mw::streamer::InputFrameSource& {
    const auto found = std::find_if(
        source.tracks.begin(), source.tracks.end(), [](const auto& track) {
          return track.stream.codec_parameters.get()->codec_type ==
                 AVMEDIA_TYPE_VIDEO;
        });
    if (found == source.tracks.end()) throw std::runtime_error("缺少视频轨道");
    return *found;
  };
  ffmpeg::Frame initial_frame;
  REQUIRE(ReceiveBefore(video_track(initial), initial_frame,
                        std::chrono::steady_clock::now() + 8s));

  std::vector<DecodedInputSource> sought;
  for (const auto position : {400ms, 405ms}) {
    const auto segment_index = sought.size() + 1;
    input.Seek(position);
    REQUIRE(capture.WaitSources(segment_index + 1));
    auto source = capture.Source(segment_index);
    CHECK(source.media_origin_us == reference.start_time_us);
    CHECK(source.start_time_us ==
          reference.start_time_us + position.count() * 1000);
    CHECK(source.duration_us ==
          reference.duration_us - position.count() * 1000);
    // Seek resets the producer, but its original media endpoint stays fixed.
    CHECK(source.start_time_us + source.duration_us - source.media_origin_us ==
          reference.duration_us);
    CHECK_FALSE(source.loop);
    CHECK(source.flush);
    REQUIRE(source.complete);
    const auto& track = video_track(source);
    ffmpeg::Frame frame;
    REQUIRE(ReceiveBefore(track, frame, std::chrono::steady_clock::now() + 8s));
    const auto& raw_frames = reference.frames.at(track.stream.stream_index);
    const auto original = std::find_if(
        raw_frames.begin(), raw_frames.end(), [&](const auto& candidate) {
          return candidate->best_effort_timestamp == frame->pts;
        });
    REQUIRE(original != raw_frames.end());
    CheckRawPullFrame(frame, *original, track.stream.time_base);
    // Decoding starts at a preceding keyframe. The filter performs the seek
    // cut; Input neither subtracts 400/405 ms nor rebases raw media PTS.
    CHECK(frame->pts <= av_rescale_q(source.start_time_us, AV_TIME_BASE_Q,
                                     track.stream.time_base));
    sought.push_back(std::move(source));
  }
  CHECK(sought[1].start_time_us - sought[0].start_time_us == 5000);
  for (const auto& track : initial.tracks) CHECK(track.finished());
  for (const auto& track : sought.front().tracks) CHECK(track.finished());
  const auto& last = sought.back();
  std::map<int, std::vector<ffmpeg::Frame>> remaining;
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  for (;;) {
    bool finished = true;
    bool progress = false;
    for (const auto& track : last.tracks) {
      ffmpeg::Frame frame;
      if (track.try_receive(frame)) {
        remaining[track.stream.stream_index].push_back(frame.Ref());
        progress = true;
      }
      finished &= track.finished();
    }
    if (finished) break;
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    if (!progress) std::this_thread::sleep_for(1ms);
  }
  // One video frame from the final source was already consumed above.
  last.complete(PulledFrameCount(last, remaining) + 1);
  REQUIRE(capture.WaitState(InputState::kEnded));
  input.Stop();
  CHECK_NOTHROW(sought.front().complete(0));
  CHECK_NOTHROW(initial.complete(0));
}

}  // namespace

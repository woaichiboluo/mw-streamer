#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mw/streamer/input/ffmpeg_input.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};

std::string SpeedSamplePath(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + name;
}

struct ReferenceTrack {
  AVRational time_base;
  std::vector<ffmpeg::Frame> frames;
};

// Decode independently of Input so dropped frames and changed media positions
// cannot be concealed by comparing two Input implementations.
std::map<int, ReferenceTrack> DecodeSpeedReference(
    const std::string& path,
    std::optional<std::chrono::milliseconds> seek = std::nullopt) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "open reference");
  const auto close = [](AVFormatContext* context) {
    avformat_close_input(&context);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "read reference streams");
  std::map<int, ffmpeg::CodecContext> decoders;
  std::map<int, ReferenceTrack> result;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int index = av_find_best_stream(raw, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) continue;
    ffmpeg::FfmpegException::throwIfError(index, "select reference stream");
    const auto* stream = raw->streams[index];
    ffmpeg::CodecContext decoder(
        avcodec_find_decoder(stream->codecpar->codec_id));
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(decoder.get(), stream->codecpar),
        "configure reference decoder");
    decoder.get()->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr),
        "open reference decoder");
    decoders.emplace(index, std::move(decoder));
    result.emplace(index, ReferenceTrack{stream->time_base, {}});
  }
  if (seek) {
    const int index =
        av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    auto target = av_rescale_q(seek->count(), {1, 1000}, AV_TIME_BASE_Q);
    if (raw->start_time != AV_NOPTS_VALUE) target += raw->start_time;
    target =
        av_rescale_q(target, AV_TIME_BASE_Q, raw->streams[index]->time_base);
    ffmpeg::FfmpegException::throwIfError(
        av_seek_frame(raw, index, target, AVSEEK_FLAG_BACKWARD),
        "seek reference");
  }
  const auto receive = [&](int index, ffmpeg::CodecContext& decoder) {
    ffmpeg::Frame frame;
    for (;;) {
      const int status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(status, "decode reference frame");
      result.at(index).frames.push_back(frame.Ref());
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "read reference packet");
    const auto decoder = decoders.find(packet->stream_index);
    if (decoder == decoders.end()) continue;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder->second.get(), packet.get()),
        "send reference packet");
    receive(decoder->first, decoder->second);
  }
  for (auto& [index, decoder] : decoders) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder.get(), nullptr), "drain reference decoder");
    receive(index, decoder);
  }
  return result;
}

struct SpeedFrame {
  int index;
  std::uint64_t generation;
  ffmpeg::Frame frame;
  Clock::time_point received;
};

class SpeedCollector final {
 public:
  void Attach(FfmpegInput& input,
              std::function<void(const ffmpeg::Frame&)> frame_hook = {}) {
    input.SetOnReady([this](const auto& streams) {
      stream_info = streams;
      ++ready;
    });
    input.SetOnPacket([this](std::uint64_t generation, const auto&) {
      generation_ = generation;
    });
    input.SetOnFrame([this, frame_hook](int index, const auto& frame) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        frames.push_back({index, generation_, frame.Ref(), Clock::now()});
      }
      changed_.notify_all();
      if (frame_hook) frame_hook(frame);
    });
    input.SetOnStateChanged([this](InputState state, int, std::string_view) {
      std::lock_guard<std::mutex> lock(mutex_);
      ended_ |= state == InputState::kEnded;
      failed |= state == InputState::kFailed;
      changed_.notify_all();
    });
  }

  bool WaitForEnd() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 10s, [&] { return ended_ || failed; });
  }

  bool WaitForFrames(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 3s,
                             [&] { return frames.size() >= count || failed; });
  }

  // Read only after Stop joins the callback worker.
  std::vector<SpeedFrame> frames;
  std::vector<ffmpeg::StreamInfo> stream_info;
  int ready = 0;
  bool failed = false;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t generation_ = 0;
  bool ended_ = false;
};

std::int64_t ScaleNs(std::int64_t value, double speed) {
  return static_cast<std::int64_t>(
      std::llround(static_cast<double>(value) / speed));
}

std::int64_t MediaNs(const SpeedFrame& value,
                     const std::map<int, ReferenceTrack>& reference) {
  return av_rescale_q(value.frame->best_effort_timestamp,
                      reference.at(value.index).time_base, kNanoseconds);
}

TEST_CASE("Input playback speed validates finite supported values",
          "[input][speed]") {
  FfmpegInputConfig config;
  CHECK(config.playback_speed == 1.0);
  config.playback_speed =
      GENERATE(0.0, -1.0, 0.499, 8.001, std::numeric_limits<double>::infinity(),
               -std::numeric_limits<double>::infinity(),
               std::numeric_limits<double>::quiet_NaN());
  CHECK_THROWS_AS(FfmpegInput{config}, std::invalid_argument);
  config.decode = false;
  CHECK_THROWS_AS(FfmpegInput{config}, std::invalid_argument);
}

TEST_CASE("Input playback speed scales decoded timelines and delivery",
          "[input][speed]") {
  const double speed = GENERATE(0.5, 1.0, 2.0, 4.0, 8.0, 1.234567);
  CAPTURE(speed);
  const auto reference = DecodeSpeedReference(SpeedSamplePath());
  REQUIRE(reference.size() == 2);
  SpeedCollector collector;
  FfmpegInputConfig config;
  config.playback_speed = speed;
  FfmpegInput input(config);
  collector.Attach(input);
  input.Start(SpeedSamplePath());
  REQUIRE(collector.WaitForEnd());
  input.Stop();
  REQUIRE_FALSE(collector.failed);
  REQUIRE_FALSE(collector.frames.empty());
  CHECK(collector.ready == 1);
  const auto& first = collector.frames.front();
  const auto offset =
      first.frame->pts - ScaleNs(MediaNs(first, reference), speed);
  std::map<int, std::size_t> positions;
  for (const auto& value : collector.frames) {
    const auto& track = reference.at(value.index);
    const auto position = positions[value.index]++;
    REQUIRE(position < track.frames.size());
    const auto& expected = track.frames[position];
    CHECK(value.generation == 0);
    CHECK(value.frame->best_effort_timestamp ==
          expected->best_effort_timestamp);
    CHECK(av_cmp_q(value.frame->time_base, kNanoseconds) == 0);
    CHECK(value.frame->pkt_dts == AV_NOPTS_VALUE);
    CHECK(std::abs(value.frame->pts -
                   ScaleNs(MediaNs(value, reference), speed) - offset) <= 2);
    REQUIRE(expected->duration > 0);
    const auto duration_ns =
        av_rescale_q(expected->duration, track.time_base, kNanoseconds);
    CHECK(std::abs(value.frame->duration - ScaleNs(duration_ns, speed)) <= 2);
    CHECK(value.frame->nb_samples == expected->nb_samples);
    if (expected->sample_rate > 0) {
      CHECK(value.frame->sample_rate ==
            static_cast<int>(std::llround(expected->sample_rate * speed)));
    }
  }
  for (const auto& [index, track] : reference) {
    CHECK(positions[index] == track.frames.size());
    const auto stream = std::find_if(
        collector.stream_info.begin(), collector.stream_info.end(),
        [index](const auto& info) { return info.stream_index == index; });
    REQUIRE(stream != collector.stream_info.end());
    CHECK(av_cmp_q(stream->time_base, track.time_base) == 0);
    if (track.frames.front()->sample_rate > 0) {
      CHECK(stream->codec_parameters.get()->sample_rate ==
            track.frames.front()->sample_rate);
    }
  }
  const auto& last = collector.frames.back();
  const double expected_elapsed =
      static_cast<double>(MediaNs(last, reference) -
                          MediaNs(first, reference)) /
      speed / 1000000000.0;
  const double elapsed =
      std::chrono::duration<double>(last.received - first.received).count();
  // The generous upper bound permits loaded CI runners. The lower bound
  // catches unpaced reading and accidental double application of speed.
  CHECK(elapsed >= expected_elapsed - 0.04);
  CHECK(elapsed < expected_elapsed + 0.6);
}

TEST_CASE("Accelerated Input Seek retains media positions and scaled mapping",
          "[input][speed][seek]") {
  const auto path = SpeedSamplePath("seek_h264_aac.mp4");
  const auto reference = DecodeSpeedReference(path, 6s);
  SpeedCollector collector;
  FfmpegInputConfig config;
  config.playback_speed = 8.0;
  FfmpegInput input(config);
  bool requested = false;
  collector.Attach(input, [&](const auto& frame) {
    if (frame->width > 0 && !requested) {
      requested = true;
      input.Seek(6s);
    }
  });
  input.Start(path);
  REQUIRE(collector.WaitForEnd());
  input.Stop();
  REQUIRE_FALSE(collector.failed);
  REQUIRE(requested);
  REQUIRE_FALSE(collector.frames.empty());
  const auto& first = collector.frames.front();
  const auto offset =
      first.frame->pts - ScaleNs(MediaNs(first, reference), 8.0);
  std::map<int, std::size_t> positions;
  for (const auto& value : collector.frames) {
    CHECK(std::abs(value.frame->pts - ScaleNs(MediaNs(value, reference), 8.0) -
                   offset) <= 2);
    if (value.generation != 1) continue;
    const auto& track = reference.at(value.index);
    const auto position = positions[value.index]++;
    REQUIRE(position < track.frames.size());
    CHECK(value.frame->best_effort_timestamp ==
          track.frames[position]->best_effort_timestamp);
  }
  for (const auto& [index, track] : reference) {
    CHECK(positions[index] == track.frames.size());
  }
  CHECK(collector.ready == 1);
}

TEST_CASE("Accelerated Input loops scale final duration and output continuity",
          "[input][speed][loop]") {
  SpeedCollector collector;
  FfmpegInputConfig config;
  config.playback_speed = 8.0;
  config.loop = true;
  FfmpegInput input(config);
  collector.Attach(input);
  input.Start(SpeedSamplePath("seek_tail_h264.mp4"));
  REQUIRE(collector.WaitForFrames(2));
  input.Stop();
  REQUIRE_FALSE(collector.failed);
  REQUIRE(collector.frames.size() >= 2);
  const auto& first = collector.frames[0];
  const auto& second = collector.frames[1];
  CHECK(first.generation == 0);
  CHECK(second.generation == 1);
  CHECK(first.frame->best_effort_timestamp == 0);
  CHECK(second.frame->best_effort_timestamp == 0);
  CHECK(first.frame->duration == 250000000);
  CHECK(second.frame->duration == 250000000);
  CHECK(second.frame->pts - first.frame->pts == 250000000);
  const auto elapsed = second.received - first.received;
  CHECK(elapsed >= 230ms);
  CHECK(elapsed < 800ms);
  CHECK(collector.ready == 1);
}

}  // namespace

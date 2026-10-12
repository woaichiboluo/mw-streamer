#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/input/ffmpeg_input.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputMode;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string DecodeSamplePath(const std::string& name = "h264_aac.mp4") {
  return std::string(MW_STREAMER_INPUT_TEST_DATA_DIR) + "/" + name;
}

struct ReferenceTrack {
  AVRational time_base;
  std::vector<ffmpeg::Frame> frames;
};

// Decode independently of Input so dropped frames and changed media positions
// cannot be concealed by comparing two Input implementations.
std::map<int, ReferenceTrack> DecodeReference(
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
    decoder.get()->thread_count = 0;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr),
        "open reference decoder");
    decoders.emplace(index, std::move(decoder));
    result.emplace(index, ReferenceTrack{stream->time_base, {}});
  }
  if (seek) {
    const auto target =
        av_rescale_q(seek->count(), AVRational{1, 1000}, AV_TIME_BASE_Q) +
        (raw->start_time == AV_NOPTS_VALUE ? 0 : raw->start_time);
    ffmpeg::FfmpegException::throwIfError(
        avformat_seek_file(raw, -1, INT64_MIN, target, target, 0),
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

struct DecodedFrame {
  int index;
  std::uint64_t generation;
  ffmpeg::Frame frame;
  Clock::time_point received;
};

class FrameCollector final {
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
  std::vector<DecodedFrame> frames;
  std::vector<ffmpeg::StreamInfo> stream_info;
  int ready = 0;
  bool failed = false;

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t generation_ = 0;
  bool ended_ = false;
};

void CheckDecodedFrame(const DecodedFrame& value, const ReferenceTrack& track,
                       const ffmpeg::Frame& expected) {
  CHECK(value.frame->best_effort_timestamp == expected->best_effort_timestamp);
  CHECK(value.frame->pts == expected->best_effort_timestamp);
  CHECK(av_cmp_q(value.frame->time_base, track.time_base) == 0);
  CHECK(value.frame->pkt_dts == expected->pkt_dts);
  CHECK(value.frame->duration == expected->duration);
  REQUIRE(value.frame->nb_samples == expected->nb_samples);
  REQUIRE(value.frame->format == expected->format);
  CHECK(value.frame->width == expected->width);
  CHECK(value.frame->height == expected->height);
  CHECK(value.frame->color_range == expected->color_range);
  CHECK(value.frame->colorspace == expected->colorspace);
  if (expected->sample_rate <= 0) return;
  CHECK(value.frame->sample_rate == expected->sample_rate);
  REQUIRE(av_channel_layout_compare(&value.frame->ch_layout,
                                    &expected->ch_layout) == 0);
}

void CheckAudioSamples(const ffmpeg::Frame& actual,
                       const ffmpeg::Frame& expected) {
  if (expected->sample_rate <= 0) return;
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

void CheckVideoPixels(const ffmpeg::Frame& actual,
                      const ffmpeg::Frame& expected) {
  if (expected->width <= 0) return;
  const auto format = static_cast<AVPixelFormat>(expected->format);
  const int bytes =
      av_image_get_buffer_size(format, expected->width, expected->height, 1);
  REQUIRE(bytes > 0);
  std::vector<std::uint8_t> actual_pixels(static_cast<std::size_t>(bytes));
  std::vector<std::uint8_t> expected_pixels(static_cast<std::size_t>(bytes));
  REQUIRE(av_image_copy_to_buffer(actual_pixels.data(), bytes, actual->data,
                                  actual->linesize, format, actual->width,
                                  actual->height, 1) == bytes);
  REQUIRE(av_image_copy_to_buffer(expected_pixels.data(), bytes, expected->data,
                                  expected->linesize, format, expected->width,
                                  expected->height, 1) == bytes);
  CHECK(actual_pixels == expected_pixels);
}

TEST_CASE("Input 模式默认 Live 并拒绝未知取值", "[input][mode]") {
  FfmpegInputConfig config;
  CHECK(config.mode == InputMode::kLive);
  config.mode = static_cast<InputMode>(-1);
  CHECK_THROWS_AS(FfmpegInput{config}, std::invalid_argument);
  config.mode = static_cast<InputMode>(3);
  CHECK_THROWS_AS(FfmpegInput{config}, std::invalid_argument);
}

TEST_CASE("Input 解码交付保留源时间基和原始音视频内容",
          "[input][mode][decode][native]") {
  const auto mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  CAPTURE(static_cast<int>(mode));
  const auto reference = DecodeReference(DecodeSamplePath());
  REQUIRE(reference.size() == 2);
  FrameCollector collector;
  FfmpegInputConfig config;
  config.mode = mode;
  {
    FfmpegInput input(config);
    collector.Attach(input);
    input.Start(DecodeSamplePath());
    REQUIRE(collector.WaitForEnd());
    input.Stop();
  }
  REQUIRE_FALSE(collector.failed);
  REQUIRE_FALSE(collector.frames.empty());
  CHECK(collector.ready == 1);
  std::map<int, std::size_t> positions;
  // The callback retains Ref() values. Comparing after Input destruction also
  // verifies that decoder buffer reuse and shutdown do not invalidate them.
  for (const auto& value : collector.frames) {
    const auto& track = reference.at(value.index);
    const auto position = positions[value.index]++;
    REQUIRE(position < track.frames.size());
    CHECK(value.generation == 0);
    CheckDecodedFrame(value, track, track.frames[position]);
    CheckAudioSamples(value.frame, track.frames[position]);
    CheckVideoPixels(value.frame, track.frames[position]);
  }
  // Independent reference decoding drains delayed frames at EOF. Counts and
  // per-track ordering detect missing tail frames and duplicate delivery.
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
}

TEST_CASE("Input Seek 保留源位置和音视频偏移", "[input][mode][decode][seek]") {
  const auto mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  CAPTURE(static_cast<int>(mode));
  const auto path = DecodeSamplePath("seek_h264_aac.mp4");
  const auto reference = DecodeReference(path, 6s);
  FrameCollector collector;
  FfmpegInputConfig config;
  config.mode = mode;
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
  std::map<int, std::size_t> positions;
  for (const auto& value : collector.frames) {
    if (value.generation != 1) continue;
    const auto& track = reference.at(value.index);
    const auto position = positions[value.index]++;
    REQUIRE(position < track.frames.size());
    CheckDecodedFrame(value, track, track.frames[position]);
    CheckAudioSamples(value.frame, track.frames[position]);
    CheckVideoPixels(value.frame, track.frames[position]);
  }
  for (const auto& [index, track] : reference) {
    CHECK(positions[index] == track.frames.size());
  }
  CHECK(collector.ready == 2);
}

TEST_CASE("Input 单帧源不循环且保留原始尾帧时长",
          "[input][mode][decode][loop]") {
  const auto mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  CAPTURE(static_cast<int>(mode));
  const auto path = DecodeSamplePath("seek_tail_h264.mp4");
  const auto reference = DecodeReference(path);
  REQUIRE(reference.size() == 1);
  const auto& track = reference.begin()->second;
  REQUIRE(track.frames.size() == 1);
  FrameCollector collector;
  FfmpegInputConfig config;
  config.mode = mode;
  config.loop = true;
  FfmpegInput input(config);
  collector.Attach(input);
  input.Start(path);
  REQUIRE(collector.WaitForEnd());
  input.Stop();
  REQUIRE_FALSE(collector.failed);
  REQUIRE(collector.frames.size() == 1);
  CHECK(collector.frames[0].generation == 0);
  for (const auto& value : collector.frames) {
    CheckDecodedFrame(value, track, track.frames.front());
  }
  CHECK(collector.ready == 1);
}

TEST_CASE("Input 同步慢回调通过背压限制帧交付",
          "[input][mode][decode][backpressure]") {
  const auto mode = GENERATE(InputMode::kLive, InputMode::kBatch);
  CAPTURE(static_cast<int>(mode));
  FrameCollector collector;
  FfmpegInputConfig config;
  config.mode = mode;
  FfmpegInput input(config);
  collector.Attach(input,
                   [](const auto&) { std::this_thread::sleep_for(20ms); });
  input.Start(DecodeSamplePath());
  REQUIRE(collector.WaitForFrames(8));
  input.Stop();
  REQUIRE_FALSE(collector.failed);
  REQUIRE(collector.frames.size() >= 8);
  for (std::size_t index = 1; index < collector.frames.size(); ++index) {
    CHECK(collector.frames[index].received -
              collector.frames[index - 1].received >=
          20ms);
  }
}

}  // namespace

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/processor/batch_processor.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include "mw/streamer/ffmpeg/codec_context.h"
#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/input/ffmpeg_input.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::BatchProcessor;
using mw::streamer::FfmpegInput;
using mw::streamer::FfmpegInputConfig;
using mw::streamer::InputMode;
using mw::streamer::InputState;
namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kNanoseconds{1, 1000000000};

struct ReferenceTrack {
  AVRational time_base;
  std::vector<ffmpeg::Frame> frames;
};

// Decode independently, including delayed frames, to detect lost frames or
// changes to media timestamps and PCM during Input -> BatchProcessor delivery.
std::map<int, ReferenceTrack> DecodeReference(const std::string& path) {
  AVFormatContext* raw = nullptr;
  ffmpeg::FfmpegException::throwIfError(
      avformat_open_input(&raw, path.c_str(), nullptr, nullptr),
      "open batch reference");
  const auto close = [](AVFormatContext* context) {
    avformat_close_input(&context);
  };
  std::unique_ptr<AVFormatContext, decltype(close)> format(raw, close);
  ffmpeg::FfmpegException::throwIfError(avformat_find_stream_info(raw, nullptr),
                                        "find batch reference streams");
  std::map<int, ffmpeg::CodecContext> decoders;
  std::map<int, ReferenceTrack> tracks;
  for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
    const int index = av_find_best_stream(raw, type, -1, -1, nullptr, 0);
    ffmpeg::FfmpegException::throwIfError(index,
                                          "select batch reference track");
    const auto* stream = raw->streams[index];
    ffmpeg::CodecContext decoder(
        avcodec_find_decoder(stream->codecpar->codec_id));
    ffmpeg::FfmpegException::throwIfError(
        avcodec_parameters_to_context(decoder.get(), stream->codecpar),
        "configure batch reference decoder");
    decoder.get()->pkt_timebase = stream->time_base;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_open2(decoder.get(), nullptr, nullptr),
        "open batch reference decoder");
    decoders.emplace(index, std::move(decoder));
    tracks.emplace(index, ReferenceTrack{stream->time_base, {}});
  }
  const auto receive = [&](int index, ffmpeg::CodecContext& decoder) {
    ffmpeg::Frame frame;
    for (;;) {
      const int status = avcodec_receive_frame(decoder.get(), frame.get());
      if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
      ffmpeg::FfmpegException::throwIfError(status, "receive batch reference");
      tracks.at(index).frames.push_back(frame.Ref());
    }
  };
  for (;;) {
    ffmpeg::Packet packet;
    const int status = av_read_frame(raw, packet.get());
    if (status == AVERROR_EOF) break;
    ffmpeg::FfmpegException::throwIfError(status, "read batch reference");
    const auto decoder = decoders.find(packet->stream_index);
    if (decoder == decoders.end()) continue;
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder->second.get(), packet.get()),
        "send batch reference");
    receive(decoder->first, decoder->second);
  }
  for (auto& [index, decoder] : decoders) {
    ffmpeg::FfmpegException::throwIfError(
        avcodec_send_packet(decoder.get(), nullptr), "drain batch reference");
    receive(index, decoder);
  }
  return tracks;
}

TEST_CASE(
    "Batch Input synchronously delivers complete original media to consumers",
    "[processor][batch][input]") {
  const auto path =
      std::string(MW_STREAMER_BATCH_PROCESSOR_TEST_DATA_DIR) + "/h264_aac.mp4";
  const auto reference = DecodeReference(path);
  REQUIRE(reference.size() == 2);
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  FfmpegInputConfig config;
  config.mode = InputMode::kBatch;
  config.playback_speed = 8.0;
  std::mutex mutex;
  std::condition_variable changed;
  bool ended = false, failed = false, accepted = false;
  int ready = 0, end_events = 0, stop_events = 0;
  bool borrowed = false, synchronous = true;
  std::thread::id input_worker;
  int current_index = -1;
  const ffmpeg::Frame* current_frame = nullptr;
  std::vector<ffmpeg::StreamInfo> supplied_streams;
  std::map<int, std::vector<ffmpeg::Frame>> consumed;
  std::exception_ptr callback_error;
  const std::vector<ffmpeg::StreamInfo>* original_streams = nullptr;
  // Callback state outlives both modules, including early test exits.
  BatchProcessor processor;
  FfmpegInput input(cpu, config);
  processor.SetOnReady([&](const auto& streams, const auto& device) noexcept {
    ++ready;
    borrowed = &streams == original_streams && &device == &cpu &&
               device.get() == nullptr;
    try {
      supplied_streams = streams;
      return true;
    } catch (...) {
      callback_error = std::current_exception();
      return false;
    }
  });
  const auto consume = [&](const ffmpeg::Frame& frame) noexcept {
    synchronous &=
        &frame == current_frame && std::this_thread::get_id() == input_worker;
    try {
      consumed[current_index].push_back(frame.Ref());
      // A slow synchronous consumer applies backpressure without an internal
      // processor worker or queue. Keep it short so the test remains quick.
      std::this_thread::sleep_for(2ms);
    } catch (...) {
      callback_error = std::current_exception();
    }
  };
  processor.SetOnVideo(consume);
  processor.SetOnAudio(consume);
  processor.SetOnEnded([&] { ++end_events; });
  processor.SetOnStop([&] { ++stop_events; });
  input.SetOnReady([&](const auto& streams) {
    original_streams = &streams;
    input_worker = std::this_thread::get_id();
    accepted = processor.Start(streams, cpu);
  });
  input.SetOnFrame([&](int index, const auto& frame) {
    if (!accepted) return;
    current_index = index;
    current_frame = &frame;
    if (frame->nb_samples > 0) {
      processor.ProcessAudio(frame);
    } else {
      processor.ProcessVideo(frame);
    }
    current_frame = nullptr;
  });
  input.SetOnStateChanged([&](InputState state, int, std::string_view) {
    std::lock_guard<std::mutex> lock(mutex);
    ended |= state == InputState::kEnded;
    failed |= state == InputState::kFailed;
    changed.notify_all();
  });
  input.Start(path);
  bool completed;
  {
    std::unique_lock<std::mutex> lock(mutex);
    completed = changed.wait_for(lock, 10s, [&] { return ended || failed; });
  }
  input.Stop();
  processor.End();
  processor.Stop();
  REQUIRE(completed);
  REQUIRE_FALSE(failed);
  REQUIRE_FALSE(callback_error);
  REQUIRE(accepted);
  CHECK(ready == 1);
  CHECK(end_events == 1);
  CHECK(stop_events == 1);
  CHECK(borrowed);
  CHECK(synchronous);
  REQUIRE(supplied_streams.size() == reference.size());
  for (const auto& stream : supplied_streams) {
    const auto& track = reference.at(stream.stream_index);
    REQUIRE_FALSE(track.frames.empty());
    CHECK(av_cmp_q(stream.time_base, track.time_base) == 0);
    const auto& expected = track.frames.front();
    const auto* parameters = stream.codec_parameters.get();
    CHECK(parameters->width == expected->width);
    CHECK(parameters->height == expected->height);
    CHECK(parameters->sample_rate == expected->sample_rate);
  }
  for (const auto& [index, track] : reference) {
    const auto& frames = consumed.at(index);
    REQUIRE(frames.size() == track.frames.size());
    for (std::size_t position = 0; position < frames.size(); ++position) {
      const auto& actual = frames[position];
      const auto& expected = track.frames[position];
      REQUIRE(expected->best_effort_timestamp != AV_NOPTS_VALUE);
      CHECK(actual->best_effort_timestamp == expected->best_effort_timestamp);
      CHECK(actual->pts == av_rescale_q(expected->best_effort_timestamp,
                                        track.time_base, kNanoseconds));
      CHECK(av_cmp_q(actual->time_base, kNanoseconds) == 0);
      CHECK(actual->pkt_dts == AV_NOPTS_VALUE);
      CHECK(actual->duration ==
            av_rescale_q(expected->duration, track.time_base, kNanoseconds));
      CHECK(actual->width == expected->width);
      CHECK(actual->height == expected->height);
      REQUIRE(actual->format == expected->format);
      CHECK(actual->sample_rate == expected->sample_rate);
      REQUIRE(actual->nb_samples == expected->nb_samples);
      if (expected->nb_samples == 0) continue;
      REQUIRE(av_channel_layout_compare(&actual->ch_layout,
                                        &expected->ch_layout) == 0);
      const auto sample_format = static_cast<AVSampleFormat>(expected->format);
      const bool planar = av_sample_fmt_is_planar(sample_format) != 0;
      const int planes = planar ? expected->ch_layout.nb_channels : 1;
      const int bytes = expected->nb_samples *
                        av_get_bytes_per_sample(sample_format) *
                        (planar ? 1 : expected->ch_layout.nb_channels);
      REQUIRE(bytes > 0);
      for (int plane = 0; plane < planes; ++plane) {
        CHECK(std::memcmp(actual->extended_data[plane],
                          expected->extended_data[plane],
                          static_cast<std::size_t>(bytes)) == 0);
      }
    }
  }
}

}  // namespace

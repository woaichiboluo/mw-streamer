#include "mw/streamer/input/av_media_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoderReadStatus;
using mw::streamer::AvFrameSourceReadStatus;
using mw::streamer::AvMediaInput;
using mw::streamer::AvMediaInputConfig;
using mw::streamer::AvMediaInputMode;
using mw::streamer::AvMediaInputState;
using mw::streamer::AvMediaInputStatus;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return fmt::format("{}/{}", MW_AV_MEDIA_INPUT_TEST_DATA_DIR, name);
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct FormatDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct CodecDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct Track {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;
};

struct NativeSource {
  std::optional<Track> video;
  std::optional<Track> audio;
};

NativeSource ReadNative(const std::string& path) {
  AVFormatContext* context = nullptr;
  const int opened =
      avformat_open_input(&context, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(opened >= 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  NativeSource result;
  for (unsigned int index = 0; index < context->nb_streams; ++index) {
    const auto* stream = context->streams[index];
    const auto type = stream->codecpar->codec_type;
    auto* selected = type == AVMEDIA_TYPE_VIDEO ? &result.video : &result.audio;
    if ((type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) || *selected)
      continue;
    Track track;
    track.stream = {stream->index, ffmpeg::CodecParameters(*stream->codecpar),
                    stream->time_base};
    track.stream.frame_rate =
        av_guess_frame_rate(context, context->streams[index], nullptr);
    track.stream.sample_aspect_ratio =
        av_guess_sample_aspect_ratio(context, context->streams[index], nullptr);
    *selected = std::move(track);
  }
  for (;;) {
    ffmpeg::Packet packet;
    const int code = av_read_frame(context, packet.get());
    if (code == AVERROR_EOF) break;
    REQUIRE(code >= 0);
    if (result.video &&
        packet->stream_index == result.video->stream.stream_index) {
      result.video->packets.push_back(std::move(packet));
    } else if (result.audio &&
               packet->stream_index == result.audio->stream.stream_index) {
      result.audio->packets.push_back(std::move(packet));
    }
  }
  return result;
}

struct FrameRecord {
  std::int64_t pts = AV_NOPTS_VALUE;
  int format = -1;
  int width = 0;
  int height = 0;
  int samples = 0;
  int sample_rate = 0;
  int channels = 0;
  std::vector<std::uint8_t> bytes;
  bool operator==(const FrameRecord& other) const {
    return pts == other.pts && format == other.format && width == other.width &&
           height == other.height && samples == other.samples &&
           sample_rate == other.sample_rate && channels == other.channels &&
           bytes == other.bytes;
  }
};

FrameRecord Record(const AVFrame& frame) {
  FrameRecord record;
  record.pts = frame.pts;
  record.format = frame.format;
  record.width = frame.width;
  record.height = frame.height;
  record.samples = frame.nb_samples;
  record.sample_rate = frame.sample_rate;
  record.channels = frame.ch_layout.nb_channels;
  if (frame.nb_samples > 0) {
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? frame.ch_layout.nb_channels : 1;
    const int bytes = frame.nb_samples * av_get_bytes_per_sample(format) *
                      (planar ? 1 : frame.ch_layout.nb_channels);
    for (int plane = 0; plane < planes; ++plane) {
      record.bytes.insert(record.bytes.end(), frame.extended_data[plane],
                          frame.extended_data[plane] + bytes);
    }
  } else {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const int size =
        av_image_get_buffer_size(format, frame.width, frame.height, 1);
    REQUIRE(size > 0);
    record.bytes.resize(static_cast<std::size_t>(size));
    REQUIRE(av_image_copy_to_buffer(record.bytes.data(), size, frame.data,
                                    frame.linesize, format, frame.width,
                                    frame.height, 1) == size);
  }
  return record;
}

void CheckRecord(const FrameRecord& actual, const FrameRecord& expected) {
  CHECK(actual.pts == expected.pts);
  CHECK(actual.format == expected.format);
  CHECK(actual.width == expected.width);
  CHECK(actual.height == expected.height);
  CHECK(actual.samples == expected.samples);
  CHECK(actual.sample_rate == expected.sample_rate);
  CHECK(actual.channels == expected.channels);
  REQUIRE(actual.bytes.size() == expected.bytes.size());
  if (!actual.bytes.empty()) {
    CHECK(std::memcmp(actual.bytes.data(), expected.bytes.data(),
                      actual.bytes.size()) == 0);
  }
}

std::vector<std::vector<FrameRecord>> DecodeNativeGenerations(
    const Track& track, std::size_t generations) {
  const auto* codec =
      avcodec_find_decoder(track.stream.codec_parameters.get()->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecDeleter> context(
      avcodec_alloc_context3(codec));
  REQUIRE(context != nullptr);
  REQUIRE(avcodec_parameters_to_context(
              context.get(), track.stream.codec_parameters.get()) >= 0);
  context->pkt_timebase = track.stream.time_base;
  context->framerate = track.stream.frame_rate;
  context->thread_count = 1;
  REQUIRE(avcodec_open2(context.get(), codec, nullptr) >= 0);
  std::vector<FrameRecord> records;
  const auto receive = [&] {
    for (;;) {
      ffmpeg::Frame frame;
      const int code = avcodec_receive_frame(context.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      frame->pts = frame->best_effort_timestamp;
      records.push_back(Record(*frame.get()));
    }
  };
  std::vector<std::vector<FrameRecord>> output;
  for (std::size_t generation = 0; generation < generations; ++generation) {
    // Native flush retains codec state such as AAC's PNS random sequence.
    // Looping input reuses its codec, so reopening is not an equivalent oracle.
    if (generation != 0) avcodec_flush_buffers(context.get());
    for (const auto& packet : track.packets) {
      REQUIRE(avcodec_send_packet(context.get(), packet.get()) >= 0);
      REQUIRE(receive() == AVERROR(EAGAIN));
    }
    REQUIRE(avcodec_send_packet(context.get(), nullptr) >= 0);
    REQUIRE(receive() == AVERROR_EOF);
    output.push_back(std::move(records));
    records.clear();
  }
  return output;
}

std::vector<FrameRecord> DecodeNative(const Track& track) {
  auto output = DecodeNativeGenerations(track, 1);
  return std::move(output.front());
}

AvMediaInputConfig BatchConfig(bool loop = false) {
  AvMediaInputConfig config;
  config.mode = AvMediaInputMode::kBatch;
  config.proxy.loop = loop;
  config.proxy.input.packet_capacity = 2;
  config.video_decoder.thread_count = 1;
  config.audio_decoder.thread_count = 1;
  return config;
}

void CheckBatch(AvMediaInput& input, const NativeSource& reference,
                std::size_t generations = 1) {
  const auto video =
      reference.video ? DecodeNativeGenerations(*reference.video, generations)
                      : std::vector<std::vector<FrameRecord>>(generations);
  const auto audio =
      reference.audio ? DecodeNativeGenerations(*reference.audio, generations)
                      : std::vector<std::vector<FrameRecord>>(generations);
  std::vector<std::array<std::size_t, 2>> counts(generations);
  std::vector<std::array<bool, 2>> ended(generations);
  std::optional<std::uint64_t> first_generation;
  const auto consume = [&](bool video_track) {
    const auto result =
        video_track ? input.TryReadVideo() : input.TryReadAudio();
    if (result.status == AvDecoderReadStatus::kNull) return false;
    if (!first_generation) first_generation = result.generation;
    REQUIRE(result.generation >= *first_generation);
    REQUIRE(result.generation - *first_generation < generations);
    const auto generation =
        static_cast<std::size_t>(result.generation - *first_generation);
    const std::size_t index = video_track ? 0 : 1;
    const auto& expected = video_track ? video[generation] : audio[generation];
    const auto& track = video_track ? reference.video : reference.audio;
    REQUIRE(track.has_value());
    CHECK_FALSE(ended[generation][index]);
    if (generation > 0) CHECK(ended[generation - 1][index]);
    CHECK(result.stream_index == track->stream.stream_index);
    CHECK(result.time_base.num == track->stream.time_base.num);
    CHECK(result.time_base.den == track->stream.time_base.den);
    if (result.status == AvDecoderReadStatus::kEof) {
      CHECK(counts[generation][index] == expected.size());
      CHECK_FALSE(result.frame.has_value());
      ended[generation][index] = true;
    } else {
      REQUIRE(result.status == AvDecoderReadStatus::kFrame);
      REQUIRE(result.frame.has_value());
      REQUIRE(counts[generation][index] < expected.size());
      CAPTURE(generation, index, counts[generation][index]);
      CheckRecord(Record(*result.frame->get()),
                  expected[counts[generation][index]++]);
    }
    return true;
  };
  const bool finished = WaitFor([&] {
    REQUIRE(input.status().state != AvMediaInputState::kFailed);
    bool progress = false;
    if (reference.video && !ended.back()[0]) progress |= consume(true);
    if (reference.audio && !ended.back()[1]) progress |= consume(false);
    static_cast<void>(progress);
    return (!reference.video || ended.back()[0]) &&
           (!reference.audio || ended.back()[1]);
  });
  REQUIRE(finished);
}

TEST_CASE(
    "AvMediaInput Batch preserves source pixels, samples, PTS and time bases",
    "[av-media-input][batch][oracle]") {
  const auto reference = ReadNative(SamplePath());
  auto config = BatchConfig();
  config.output.fps = {25, 1};
  config.output.audio_sample_rate = 44100;
  config.output.audio_sample_format = AV_SAMPLE_FMT_S32;
  AvMediaInput input(config);
  CHECK(input.mode() == AvMediaInputMode::kBatch);
  input.Start(SamplePath());
  CheckBatch(input, reference);
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kEnded; }));
  input.Stop();
  CHECK(input.status().state == AvMediaInputState::kEnded);
}

class TemporaryMedia final {
 public:
  TemporaryMedia() {
    static std::atomic<unsigned int> sequence{0};
    path_ =
        std::filesystem::temp_directory_path() /
        fmt::format("mw-media-input-{}-{}.wav",
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    sequence.fetch_add(1));
  }
  ~TemporaryMedia() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }
  std::string path() const { return path_.string(); }
  void RestoreVideo() {
    std::filesystem::copy_file(SamplePath("h264_video.mp4"), path_);
  }
  void CreateAudio() {
    struct OutputDeleter {
      void operator()(AVFormatContext* context) const {
        if (context->pb) avio_closep(&context->pb);
        avformat_free_context(context);
      }
    };
    AVFormatContext* raw = nullptr;
    const int allocated =
        avformat_alloc_output_context2(&raw, nullptr, "wav", path().c_str());
    std::unique_ptr<AVFormatContext, OutputDeleter> output(raw);
    REQUIRE(allocated >= 0);
    REQUIRE(output != nullptr);
    auto* stream = avformat_new_stream(output.get(), nullptr);
    REQUIRE(stream != nullptr);
    stream->time_base = {1, 44100};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->sample_rate = 44100;
    stream->codecpar->format = AV_SAMPLE_FMT_S16;
    stream->codecpar->bits_per_coded_sample = 16;
    stream->codecpar->block_align = 2;
    av_channel_layout_default(&stream->codecpar->ch_layout, 1);
    REQUIRE(avio_open(&output->pb, path().c_str(), AVIO_FLAG_WRITE) >= 0);
    REQUIRE(avformat_write_header(output.get(), nullptr) >= 0);
    ffmpeg::Packet packet;
    REQUIRE(av_new_packet(packet.get(), 4410 * 2) >= 0);
    packet->stream_index = stream->index;
    packet->pts = 0;
    packet->dts = 0;
    packet->duration = 4410;
    for (int index = 0; index < 4410; ++index) {
      const auto value = static_cast<std::uint16_t>(
          static_cast<std::int16_t>(index % 127 - 63));
      packet->data[index * 2] = static_cast<std::uint8_t>(value & 255);
      packet->data[index * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    REQUIRE(av_interleaved_write_frame(output.get(), packet.get()) >= 0);
    REQUIRE(av_write_trailer(output.get()) >= 0);
  }

 private:
  std::filesystem::path path_;
};

TEST_CASE("AvMediaInput Live emits configured FLTP audio and FPS video bundles",
          "[av-media-input][live]") {
  int fps = 30;
  SECTION("30 FPS with 1600 audio samples") {}
  SECTION("25 FPS with 1920 audio samples") { fps = 25; }
  AvMediaInputConfig config;
  config.output.fps = {fps, 1};
  config.video_decoder.thread_count = 1;
  config.audio_decoder.thread_count = 1;
  AvMediaInput input(config);
  input.Start(SamplePath());
  std::size_t count = 0;
  bool eof = false;
  bool short_tail = false;
  const bool finished = WaitFor([&] {
    REQUIRE(input.status().state != AvMediaInputState::kFailed);
    const auto result = input.TryReadFrame();
    if (result.status == AvFrameSourceReadStatus::kNull) return false;
    CHECK(result.generation == 0);
    if (result.status == AvFrameSourceReadStatus::kEof) {
      CHECK(result.frame_index == count);
      eof = true;
      return true;
    }
    REQUIRE(result.video.has_value());
    REQUIRE(result.audio.has_value());
    CHECK(result.frame_index == count);
    CHECK(result.video->get()->format == AV_PIX_FMT_YUV420P);
    CHECK(result.video->get()->time_base.num == 1);
    CHECK(result.video->get()->time_base.den == fps);
    const auto& audio = *result.audio;
    CHECK(audio->format == AV_SAMPLE_FMT_FLTP);
    CHECK(audio->sample_rate == 48000);
    CHECK(audio->ch_layout.nb_channels == 2);
    CHECK(audio->nb_samples <= 48000 / fps);
    if (audio->nb_samples < 48000 / fps) short_tail = true;
    CHECK(result.pts_us == av_rescale_q(audio->pts, audio->time_base,
                                        AVRational{1, AV_TIME_BASE}));
    CHECK(result.duration_us == av_rescale_q(audio->nb_samples,
                                             AVRational{1, 48000},
                                             AVRational{1, AV_TIME_BASE}));
    ++count;
    return false;
  });
  REQUIRE(finished);
  CHECK(eof);
  CHECK(count >= static_cast<std::size_t>(fps * 2));
  CHECK(short_tail);
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kEnded; }));
}

TEST_CASE("AvMediaInput looping exposes both generations of Batch and Live EOF",
          "[av-media-input][loop][eof]") {
  bool batch = false;
  SECTION("Batch") { batch = true; }
  SECTION("Live") {}
  auto config = BatchConfig(true);
  if (!batch) config.mode = AvMediaInputMode::kLive;
  AvMediaInput input(config);
  input.Start(SamplePath());
  if (batch) {
    CheckBatch(input, ReadNative(SamplePath()), 2);
  } else {
    std::array<std::size_t, 2> counts{};
    std::array<bool, 2> ended{};
    const bool finished = WaitFor([&] {
      REQUIRE(input.status().state != AvMediaInputState::kFailed);
      const auto result = input.TryReadFrame();
      if (result.status == AvFrameSourceReadStatus::kNull) return false;
      REQUIRE(result.generation < 2);
      const auto generation = static_cast<std::size_t>(result.generation);
      CHECK_FALSE(ended[generation]);
      if (generation == 1) CHECK(ended[0]);
      if (result.status == AvFrameSourceReadStatus::kEof) {
        CHECK(result.frame_index == counts[generation]);
        ended[generation] = true;
      } else {
        REQUIRE(result.video.has_value());
        REQUIRE(result.audio.has_value());
        CHECK(result.frame_index == counts[generation]++);
      }
      return ended[1];
    });
    REQUIRE(finished);
    REQUIRE(counts[0] > 0);
    CHECK(counts[0] == counts[1]);
  }
  CHECK(input.status().state != AvMediaInputState::kEnded);
  input.Stop();
  CHECK(input.status().state == AvMediaInputState::kStopped);
}

TEST_CASE(
    "AvMediaInput selects a single video or audio track and leaves the other "
    "empty",
    "[av-media-input][single-track]") {
  bool audio_only = false;
  SECTION("video-only file") {}
  SECTION("PCM audio-only file") { audio_only = true; }
  TemporaryMedia temporary;
  if (audio_only) temporary.CreateAudio();
  const auto path =
      audio_only ? temporary.path() : SamplePath("h264_video.mp4");
  const auto reference = ReadNative(path);
  CHECK(reference.audio.has_value() == audio_only);
  CHECK(reference.video.has_value() == !audio_only);
  AvMediaInput input(BatchConfig());
  input.Start(path);
  CheckBatch(input, reference);
  CHECK((audio_only ? input.TryReadVideo() : input.TryReadAudio()).status ==
        AvDecoderReadStatus::kNull);
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kEnded; }));
  REQUIRE(input.info() != nullptr);
  CHECK(input.info()->video_stream_index == (audio_only ? -1 : 0));
  CHECK(input.info()->audio_stream_index == (audio_only ? 0 : -1));
}

TEST_CASE(
    "AvMediaInput Draining does not become Ended before all consumer EOFs",
    "[av-media-input][draining][eof]") {
  auto config = BatchConfig();
  config.video_decoder.frame_capacity = 64;
  config.audio_decoder.frame_capacity = 128;
  AvMediaInput input(config);
  input.Start(SamplePath());
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kDraining; }));
  std::this_thread::sleep_for(20ms);
  CHECK(input.status().state == AvMediaInputState::kDraining);
  bool video_eof = false;
  REQUIRE(WaitFor([&] {
    const auto result = input.TryReadVideo();
    if (result.status == AvDecoderReadStatus::kEof) video_eof = true;
    return video_eof;
  }));
  std::this_thread::sleep_for(20ms);
  CHECK(input.status().state == AvMediaInputState::kDraining);
  bool audio_eof = false;
  REQUIRE(WaitFor([&] {
    const auto result = input.TryReadAudio();
    if (result.status == AvDecoderReadStatus::kEof) audio_eof = true;
    return audio_eof;
  }));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kEnded; }));
}

TEST_CASE("AvMediaInput retries a missing source and recovers when it appears",
          "[av-media-input][retry][recovery]") {
  TemporaryMedia temporary;
  AvMediaInput input(BatchConfig());
  input.Start(temporary.path());
  REQUIRE(WaitFor([&] {
    return input.status().state == AvMediaInputState::kWaitingRetry;
  }));
  CHECK(input.status().retry_count == 1);
  CHECK(input.status().retry_delay == 1s);
  REQUIRE(input.status().error != nullptr);
  temporary.RestoreVideo();
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kRunning; }));
  CHECK(input.status().retry_count == 0);
  CHECK(input.status().error == nullptr);
  CheckBatch(input, ReadNative(temporary.path()));
}

TEST_CASE(
    "AvMediaInput Stop cancels retry backoff without callbacks afterwards",
    "[av-media-input][retry][stop]") {
  TemporaryMedia missing;
  std::atomic<std::size_t> callbacks{0};
  AvMediaInput input(BatchConfig());
  input.SetOnStateChanged([&](const auto&) { ++callbacks; });
  input.Start(missing.path());
  REQUIRE(WaitFor([&] {
    return input.status().state == AvMediaInputState::kWaitingRetry;
  }));
  const auto started = std::chrono::steady_clock::now();
  input.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  CHECK(input.status().state == AvMediaInputState::kStopped);
  const auto count = callbacks.load();
  std::this_thread::sleep_for(20ms);
  CHECK(callbacks.load() == count);
  CHECK(input.TryReadVideo().status == AvDecoderReadStatus::kNull);
  CHECK(input.TryReadAudio().status == AvDecoderReadStatus::kNull);
  input.Stop();
}

TEST_CASE(
    "AvMediaInput retains original asynchronous failures and publishes no fake "
    "EOF",
    "[av-media-input][error]") {
  bool missing = false;
  SECTION("unknown configured decoder") {}
  SECTION("opening fails with retries disabled") { missing = true; }
  TemporaryMedia temporary;
  auto config = BatchConfig();
  config.proxy.max_retries = 0;
  if (!missing) config.video_decoder.decoder_name = "mw_missing_decoder";
  std::exception_ptr observed;
  std::size_t failed_callbacks = 0;
  AvMediaInput input(config);
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvMediaInputState::kFailed) {
      observed = status.error;
      ++failed_callbacks;
    }
  });
  input.Start(missing ? temporary.path() : SamplePath());
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kFailed; }));
  input.Stop();
  CHECK(failed_callbacks == 1);
  CHECK(input.status().state == AvMediaInputState::kFailed);
  CHECK(observed == input.status().error);
  REQUIRE(observed != nullptr);
  CHECK(input.TryReadVideo().status == AvDecoderReadStatus::kNull);
  CHECK(input.TryReadAudio().status == AvDecoderReadStatus::kNull);
  try {
    std::rethrow_exception(observed);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() ==
          (missing ? AVERROR(ENOENT) : AVERROR_DECODER_NOT_FOUND));
  }
}

TEST_CASE(
    "AvMediaInput mode mismatches and invalid configurations fail "
    "synchronously",
    "[av-media-input][validation][mode]") {
  AvMediaInput batch(BatchConfig());
  CHECK_THROWS_AS(batch.TryReadFrame(), std::logic_error);
  CHECK(batch.TryReadVideo().status == AvDecoderReadStatus::kNull);
  AvMediaInput live;
  CHECK_THROWS_AS(live.TryReadVideo(), std::logic_error);
  CHECK_THROWS_AS(live.TryReadAudio(), std::logic_error);
  CHECK(live.TryReadFrame().status == AvFrameSourceReadStatus::kNull);
  CHECK_THROWS_AS(live.Start(""), std::invalid_argument);
  CHECK_THROWS_AS(live.Start(std::string_view("a\0b", 3)),
                  std::invalid_argument);
  AvMediaInputConfig config;
  SECTION("invalid mode") { config.mode = static_cast<AvMediaInputMode>(-1); }
  SECTION("zero packet capacity") { config.proxy.input.packet_capacity = 0; }
  SECTION("zero video frame capacity") {
    config.video_decoder.frame_capacity = 0;
  }
  SECTION("negative audio codec threads") {
    config.audio_decoder.thread_count = -1;
  }
  SECTION("invalid live FPS") { config.output.fps = {0, 1}; }
  SECTION("invalid live audio sample format") {
    config.output.audio_sample_format = AV_SAMPLE_FMT_NONE;
  }
  CHECK_THROWS_AS(AvMediaInput(config), std::invalid_argument);
}

TEST_CASE(
    "AvMediaInput serial observers can query, throw and request cancellation",
    "[av-media-input][notification]") {
  bool cancel = false;
  SECTION("observer exceptions do not stop data processing") {}
  SECTION("observer RequestStop is cooperative") { cancel = true; }
  std::mutex mutex;
  std::vector<AvMediaInputState> events;
  std::thread::id worker;
  bool serial_thread = true;
  AvMediaInput input(BatchConfig());
  input.SetOnStateChanged([&](const auto& status) {
    static_cast<void>(input.status());
    static_cast<void>(input.info());
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (events.empty()) worker = std::this_thread::get_id();
      serial_thread &= worker == std::this_thread::get_id();
      events.push_back(status.state);
    }
    if (cancel && status.state == AvMediaInputState::kRunning)
      input.RequestStop();
    throw std::runtime_error("observer failure");
  });
  input.Start(SamplePath());
  if (cancel) {
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvMediaInputState::kStopped; }));
  } else {
    CheckBatch(input, ReadNative(SamplePath()));
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvMediaInputState::kEnded; }));
  }
  input.Stop();
  std::lock_guard<std::mutex> lock(mutex);
  CHECK(serial_thread);
  CHECK(worker != std::this_thread::get_id());
  REQUIRE_FALSE(events.empty());
  CHECK(events.front() == AvMediaInputState::kConnecting);
  CHECK(events.back() ==
        (cancel ? AvMediaInputState::kStopped : AvMediaInputState::kEnded));
}

TEST_CASE(
    "AvMediaInput returned frames and info survive Stop and a fresh Start",
    "[av-media-input][ownership][restart]") {
  const auto path = SamplePath("h264_video.mp4");
  AvMediaInput input(BatchConfig());
  input.Start(path);
  mw::streamer::AvDecoderReadResult retained;
  REQUIRE(WaitFor([&] {
    retained = input.TryReadVideo();
    return retained.status == AvDecoderReadStatus::kFrame;
  }));
  REQUIRE(retained.frame.has_value());
  const auto before = Record(*retained.frame->get());
  const auto info = input.info();
  REQUIRE(info != nullptr);
  input.Stop();
  input.Stop();
  CHECK(input.TryReadVideo().status == AvDecoderReadStatus::kNull);
  CHECK(Record(*retained.frame->get()) == before);
  CHECK(info->video_stream_index == 0);
  input.Start(path);
  CHECK_THROWS_AS(input.Start(path), std::logic_error);
  CheckBatch(input, ReadNative(path));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaInputState::kEnded; }));
  CHECK(input.status().generation == 0);
  CHECK(Record(*retained.frame->get()) == before);
  CHECK(info->streams.front().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_H264);
}

FrameRecord DownloadVideo(const ffmpeg::Frame& gpu) {
  ffmpeg::Frame downloaded;
  REQUIRE(av_hwframe_transfer_data(downloaded.get(), gpu.get(), 0) >= 0);
  REQUIRE(av_frame_copy_props(downloaded.get(), gpu.get()) >= 0);
  if (downloaded->format == AV_PIX_FMT_YUV420P)
    return Record(*downloaded.get());
  REQUIRE(downloaded->format == AV_PIX_FMT_NV12);
  ffmpeg::Frame planar;
  planar->format = AV_PIX_FMT_YUV420P;
  planar->width = downloaded->width;
  planar->height = downloaded->height;
  REQUIRE(av_frame_get_buffer(planar.get(), 0) >= 0);
  REQUIRE(av_frame_copy_props(planar.get(), downloaded.get()) >= 0);
  for (int row = 0; row < planar->height; ++row) {
    std::memcpy(planar->data[0] + row * planar->linesize[0],
                downloaded->data[0] + row * downloaded->linesize[0],
                static_cast<std::size_t>(planar->width));
  }
  for (int row = 0; row < planar->height / 2; ++row) {
    for (int column = 0; column < planar->width / 2; ++column) {
      const auto* sample =
          downloaded->data[1] + row * downloaded->linesize[1] + column * 2;
      planar->data[1][row * planar->linesize[1] + column] = sample[0];
      planar->data[2][row * planar->linesize[2] + column] = sample[1];
    }
  }
  return Record(*planar.get());
}

TEST_CASE(
    "AvMediaInput CUDA Batch and Live share native devices and retain GPU "
    "frames",
    "[.][av-media-input][cuda]") {
  bool batch = false;
  SECTION("Batch") { batch = true; }
  SECTION("Live") {}
  std::vector<ffmpeg::Frame> retained;
  std::vector<FrameRecord> retained_records;
  {
    ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
    const auto* shared_device = device.get()->data;
    for (const auto name : {"h264_aac.mp4", "h264_cuda_dpb.mp4"}) {
      CAPTURE(name, batch);
      const auto native = ReadNative(SamplePath(name));
      REQUIRE(native.video.has_value());
      const auto expected = DecodeNative(*native.video);
      std::array<std::unique_ptr<AvMediaInput>, 2> inputs;
      for (std::size_t backend = 0; backend < inputs.size(); ++backend) {
        auto config = BatchConfig();
        config.mode =
            batch ? AvMediaInputMode::kBatch : AvMediaInputMode::kLive;
        config.video_decoder.decoder_name =
            backend == 0 ? "h264" : "h264_cuvid";
        config.video_decoder.thread_count = backend == 0 ? 0 : 2;
        inputs[backend] = std::make_unique<AvMediaInput>(device, config);
        inputs[backend]->Start(SamplePath(name));
      }
      std::array<std::size_t, 2> counts{};
      std::array<bool, 2> video_eof{};
      std::array<bool, 2> audio_eof{};
      if (!native.audio) audio_eof.fill(true);
      const bool finished = WaitFor([&] {
        for (std::size_t backend = 0; backend < inputs.size(); ++backend) {
          auto& input = *inputs[backend];
          REQUIRE(input.status().state != AvMediaInputState::kFailed);
          std::optional<ffmpeg::Frame> video;
          if (batch) {
            if (!video_eof[backend]) {
              auto result = input.TryReadVideo();
              if (result.status == AvDecoderReadStatus::kEof)
                video_eof[backend] = true;
              video = std::move(result.frame);
            }
            if (!audio_eof[backend]) {
              const auto result = input.TryReadAudio();
              if (result.status == AvDecoderReadStatus::kEof)
                audio_eof[backend] = true;
              if (result.frame)
                CHECK(result.frame->get()->hw_frames_ctx == nullptr);
            }
          } else if (!video_eof[backend]) {
            auto result = input.TryReadFrame();
            if (result.status == AvFrameSourceReadStatus::kEof) {
              video_eof[backend] = true;
              audio_eof[backend] = true;
            }
            if (result.audio)
              CHECK(result.audio->get()->hw_frames_ctx == nullptr);
            video = std::move(result.video);
          }
          if (!video) continue;
          REQUIRE(video->get()->format == AV_PIX_FMT_CUDA);
          REQUIRE(video->get()->hw_frames_ctx != nullptr);
          const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
              video->get()->hw_frames_ctx->data);
          REQUIRE(frames->device_ref != nullptr);
          CHECK(frames->device_ref->data == shared_device);
          auto actual = DownloadVideo(*video);
          std::size_t reference_index = counts[backend];
          if (!batch) {
            reference_index = 0;
            for (std::size_t index = 1; index < expected.size(); ++index) {
              const auto mapped = av_rescale_q(expected[index].pts,
                                               native.video->stream.time_base,
                                               video->get()->time_base);
              if (mapped > video->get()->pts) break;
              reference_index = index;
            }
          }
          REQUIRE(reference_index < expected.size());
          auto wanted = expected[reference_index];
          if (!batch) wanted.pts = actual.pts;
          CHECK(actual == wanted);
          if (counts[backend]++ == 0) {
            retained.push_back(video->Ref());
            retained_records.push_back(actual);
          }
        }
        return video_eof[0] && video_eof[1] && audio_eof[0] && audio_eof[1];
      });
      REQUIRE(finished);
      for (std::size_t backend = 0; backend < inputs.size(); ++backend) {
        CHECK(counts[backend] >= expected.size());
        if (batch) CHECK(counts[backend] == expected.size());
        REQUIRE(WaitFor([&] {
          return inputs[backend]->status().state == AvMediaInputState::kEnded;
        }));
        inputs[backend]->Stop();
      }
    }
  }
  REQUIRE(retained.size() == 4);
  for (std::size_t index = 0; index < retained.size(); ++index) {
    CHECK(DownloadVideo(retained[index]) == retained_records[index]);
  }
}

}  // namespace

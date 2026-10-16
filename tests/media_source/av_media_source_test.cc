#include "mw/streamer/input/av_media_source.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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
using mw::streamer::AvMediaSource;
using mw::streamer::AvMediaSourceConfig;
using mw::streamer::AvMediaSourceState;
namespace ffmpeg = mw::streamer::ffmpeg;

std::string SamplePath(std::string_view name = "h264_aac.mp4") {
  return fmt::format("{}/{}", MW_AV_MEDIA_SOURCE_TEST_DATA_DIR, name);
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
  std::vector<ffmpeg::Packet> packets;
};

NativeSource ReadNative(const std::string& path) {
  AVFormatContext* context = nullptr;
  const int opened =
      avformat_open_input(&context, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE((opened >= 0));
  REQUIRE((avformat_find_stream_info(context, nullptr) >= 0));
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
    REQUIRE((code >= 0));
    result.packets.push_back(packet.Ref());
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
    REQUIRE((size > 0));
    record.bytes.resize(static_cast<std::size_t>(size));
    REQUIRE((av_image_copy_to_buffer(record.bytes.data(), size, frame.data,
                                     frame.linesize, format, frame.width,
                                     frame.height, 1) == size));
  }
  return record;
}

void CheckRecord(const FrameRecord& actual, const FrameRecord& expected) {
  CHECK((actual.pts == expected.pts));
  CHECK((actual.format == expected.format));
  CHECK((actual.width == expected.width));
  CHECK((actual.height == expected.height));
  CHECK((actual.samples == expected.samples));
  CHECK((actual.sample_rate == expected.sample_rate));
  CHECK((actual.channels == expected.channels));
  REQUIRE((actual.bytes.size() == expected.bytes.size()));
  if (!actual.bytes.empty()) {
    CHECK((std::memcmp(actual.bytes.data(), expected.bytes.data(),
                       actual.bytes.size()) == 0));
  }
}

std::vector<std::vector<FrameRecord>> DecodeNativeCycles(const Track& track,
                                                         std::size_t cycles) {
  const auto* codec =
      avcodec_find_decoder(track.stream.codec_parameters.get()->codec_id);
  REQUIRE((codec != nullptr));
  std::unique_ptr<AVCodecContext, CodecDeleter> context(
      avcodec_alloc_context3(codec));
  REQUIRE((context != nullptr));
  REQUIRE((avcodec_parameters_to_context(
               context.get(), track.stream.codec_parameters.get()) >= 0));
  context->pkt_timebase = track.stream.time_base;
  context->framerate = track.stream.frame_rate;
  context->thread_count = 1;
  REQUIRE((avcodec_open2(context.get(), codec, nullptr) >= 0));
  std::vector<FrameRecord> records;
  const auto receive = [&] {
    for (;;) {
      ffmpeg::Frame frame;
      const int code = avcodec_receive_frame(context.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE((code >= 0));
      frame->pts = frame->best_effort_timestamp;
      records.push_back(Record(*frame.get()));
    }
  };
  std::vector<std::vector<FrameRecord>> output;
  for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
    // Native flush retains codec state such as AAC's PNS random sequence.
    // Looping input reuses its codec, so reopening is not an equivalent oracle.
    if (cycle != 0) avcodec_flush_buffers(context.get());
    for (const auto& packet : track.packets) {
      REQUIRE((avcodec_send_packet(context.get(), packet.get()) >= 0));
      REQUIRE((receive() == AVERROR(EAGAIN)));
    }
    REQUIRE((avcodec_send_packet(context.get(), nullptr) >= 0));
    REQUIRE((receive() == AVERROR_EOF));
    output.push_back(std::move(records));
    records.clear();
  }
  return output;
}

std::vector<FrameRecord> DecodeNative(const Track& track) {
  auto output = DecodeNativeCycles(track, 1);
  return std::move(output.front());
}

AvMediaSourceConfig SourceConfig(bool loop = false) {
  AvMediaSourceConfig config;
  config.proxy.loop = loop;
  config.proxy.input.packet_capacity = 2;
  config.video_decoder.thread_count = 1;
  config.audio_decoder.thread_count = 1;
  return config;
}

void CheckSource(AvMediaSource& input, const NativeSource& reference,
                 std::size_t cycles = 1, bool video_first = false) {
  const auto video = reference.video
                         ? DecodeNativeCycles(*reference.video, cycles)
                         : std::vector<std::vector<FrameRecord>>(cycles);
  const auto audio = reference.audio
                         ? DecodeNativeCycles(*reference.audio, cycles)
                         : std::vector<std::vector<FrameRecord>>(cycles);
  std::vector<std::array<std::size_t, 2>> counts(cycles);
  std::vector<std::array<bool, 2>> ended(cycles);
  std::array<std::size_t, 2> eof_counts{};
  const auto consume = [&](bool video_track) {
    const auto result =
        video_track ? input.TryReadVideo() : input.TryReadAudio();
    if (result.status == AvDecoderReadStatus::kNull) return false;
    const std::size_t index = video_track ? 0 : 1;
    const auto cycle = eof_counts[index];
    REQUIRE((cycle < cycles));
    const auto& expected = video_track ? video[cycle] : audio[cycle];
    const auto& track = video_track ? reference.video : reference.audio;
    REQUIRE(track.has_value());
    CHECK_FALSE(ended[cycle][index]);
    if (cycle > 0) CHECK(ended[cycle - 1][index]);
    CHECK((result.stream_index == track->stream.stream_index));
    CHECK((result.time_base.num == track->stream.time_base.num));
    CHECK((result.time_base.den == track->stream.time_base.den));
    if (result.status == AvDecoderReadStatus::kEof) {
      CHECK((counts[cycle][index] == expected.size()));
      CHECK_FALSE(result.frame.has_value());
      ended[cycle][index] = true;
      ++eof_counts[index];
    } else {
      REQUIRE((result.status == AvDecoderReadStatus::kFrame));
      REQUIRE(result.frame.has_value());
      REQUIRE((counts[cycle][index] < expected.size()));
      CAPTURE(cycle, index, counts[cycle][index]);
      CheckRecord(Record(*result.frame->get()),
                  expected[counts[cycle][index]++]);
    }
    return true;
  };
  const bool finished = WaitFor(
      [&] {
        REQUIRE((input.status().state != AvMediaSourceState::kFailed));
        bool progress = false;
        if (reference.video && !ended.back()[0]) progress |= consume(true);
        if (reference.audio && !ended.back()[1] &&
            (!video_first || !reference.video || ended.back()[0]))
          progress |= consume(false);
        static_cast<void>(progress);
        return (!reference.video || ended.back()[0]) &&
               (!reference.audio || ended.back()[1]);
      },
      std::chrono::seconds(cycles * 3 + 2));
  REQUIRE(finished);
}

class TemporaryMedia final {
 public:
  TemporaryMedia() {
    static std::atomic<unsigned int> sequence{0};
    path_ =
        std::filesystem::temp_directory_path() /
        fmt::format("mw-media-source-{}-{}.wav",
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
  void CreateOrderedAv() {
    struct OutputDeleter {
      void operator()(AVFormatContext* context) const {
        if (context->pb) avio_closep(&context->pb);
        avformat_free_context(context);
      }
    };
    AVFormatContext* raw = nullptr;
    REQUIRE((avformat_alloc_output_context2(&raw, nullptr, "nut",
                                            path().c_str()) >= 0));
    std::unique_ptr<AVFormatContext, OutputDeleter> output(raw);
    REQUIRE((output != nullptr));
    auto* video = avformat_new_stream(output.get(), nullptr);
    auto* audio = avformat_new_stream(output.get(), nullptr);
    REQUIRE((video != nullptr));
    REQUIRE((audio != nullptr));
    video->time_base = {1, 25};
    video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    video->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    video->codecpar->format = AV_PIX_FMT_GRAY8;
    video->codecpar->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_GRAY8);
    video->codecpar->width = 8;
    video->codecpar->height = 8;
    audio->time_base = {1, 48000};
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->sample_rate = 48000;
    audio->codecpar->format = AV_SAMPLE_FMT_S16;
    audio->codecpar->bits_per_coded_sample = 16;
    audio->codecpar->block_align = 2;
    av_channel_layout_default(&audio->codecpar->ch_layout, 1);
    REQUIRE((avio_open(&output->pb, path().c_str(), AVIO_FLAG_WRITE) >= 0));
    REQUIRE((avformat_write_header(output.get(), nullptr) >= 0));
    // Video precedes three separately decoded audio packets. With capacity one,
    // unread audio leaves one queued frame, one held frame and one queued
    // packet. Video can accept EOF first; audio must reject EOF and then the
    // next Reset.
    for (int index = 0; index < 4; ++index) {
      ffmpeg::Packet packet;
      const bool is_video = index == 0;
      REQUIRE((av_new_packet(packet.get(), is_video ? 64 : 16) >= 0));
      packet->stream_index = is_video ? video->index : audio->index;
      packet->pts = is_video ? 0 : (index - 1) * 8;
      packet->dts = packet->pts;
      packet->duration = is_video ? 1 : 8;
      packet->flags |= AV_PKT_FLAG_KEY;
      std::memset(packet->data, index + 1,
                  static_cast<std::size_t>(packet->size));
      REQUIRE((av_write_frame(output.get(), packet.get()) >= 0));
    }
    REQUIRE((av_write_trailer(output.get()) >= 0));
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
    REQUIRE((allocated >= 0));
    REQUIRE((output != nullptr));
    auto* stream = avformat_new_stream(output.get(), nullptr);
    REQUIRE((stream != nullptr));
    stream->time_base = {1, 44100};
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    stream->codecpar->sample_rate = 44100;
    stream->codecpar->format = AV_SAMPLE_FMT_S16;
    stream->codecpar->bits_per_coded_sample = 16;
    stream->codecpar->block_align = 2;
    av_channel_layout_default(&stream->codecpar->ch_layout, 1);
    REQUIRE((avio_open(&output->pb, path().c_str(), AVIO_FLAG_WRITE) >= 0));
    REQUIRE((avformat_write_header(output.get(), nullptr) >= 0));
    ffmpeg::Packet packet;
    REQUIRE((av_new_packet(packet.get(), 4410 * 2) >= 0));
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
    REQUIRE((av_interleaved_write_frame(output.get(), packet.get()) >= 0));
    REQUIRE((av_write_trailer(output.get()) >= 0));
  }

 private:
  std::filesystem::path path_;
};

TEST_CASE(
    "AvMediaSource selects a single video or audio track and leaves the other "
    "empty",
    "[av-media-source][single-track]") {
  bool audio_only = false;
  SECTION("video-only file") {}
  SECTION("PCM audio-only file") { audio_only = true; }
  TemporaryMedia temporary;
  if (audio_only) temporary.CreateAudio();
  const auto path =
      audio_only ? temporary.path() : SamplePath("h264_video.mp4");
  const auto reference = ReadNative(path);
  CHECK((reference.audio.has_value() == audio_only));
  CHECK((reference.video.has_value() == !audio_only));
  AvMediaSource input(SourceConfig());
  REQUIRE(input.Start(path));
  CheckSource(input, reference);
  CHECK(((audio_only ? input.TryReadVideo() : input.TryReadAudio()).status ==
         AvDecoderReadStatus::kNull));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  REQUIRE((input.info() != nullptr));
  CHECK((input.info()->video_stream_index == (audio_only ? -1 : 0)));
  CHECK((input.info()->audio_stream_index == (audio_only ? 0 : -1)));
}

TEST_CASE(
    "AvMediaSource Draining does not become Ended before all consumer EOFs",
    "[av-media-source][draining][eof]") {
  auto config = SourceConfig();
  config.video_decoder.frame_capacity = 64;
  config.audio_decoder.frame_capacity = 128;
  AvMediaSource input(config);
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kDraining; }));
  std::this_thread::sleep_for(20ms);
  CHECK((input.status().state == AvMediaSourceState::kDraining));
  bool video_eof = false;
  REQUIRE(WaitFor([&] {
    const auto result = input.TryReadVideo();
    if (result.status == AvDecoderReadStatus::kEof) video_eof = true;
    return video_eof;
  }));
  std::this_thread::sleep_for(20ms);
  CHECK((input.status().state == AvMediaSourceState::kDraining));
  bool audio_eof = false;
  REQUIRE(WaitFor([&] {
    const auto result = input.TryReadAudio();
    if (result.status == AvDecoderReadStatus::kEof) audio_eof = true;
    return audio_eof;
  }));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kEnded; }));
}

TEST_CASE("AvMediaSource retries a missing source and recovers when it appears",
          "[av-media-source][retry][recovery]") {
  TemporaryMedia temporary;
  std::atomic<bool> recovered{false};
  AvMediaSource input(SourceConfig());
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvMediaSourceState::kRunning) recovered = true;
  });
  REQUIRE(input.Start(temporary.path()));
  REQUIRE(WaitFor([&] {
    return input.status().state == AvMediaSourceState::kWaitingRetry;
  }));
  CHECK((input.status().retry_count == 1));
  CHECK((input.status().retry_delay == 1s));
  REQUIRE((input.status().error != nullptr));
  temporary.RestoreVideo();
  // A short source may already be draining when the test thread wakes.
  REQUIRE(WaitFor([&] { return recovered.load(); }));
  CHECK((input.status().retry_count == 0));
  CHECK((input.status().error == nullptr));
  CheckSource(input, ReadNative(temporary.path()));
}

TEST_CASE(
    "AvMediaSource Stop cancels retry backoff without callbacks afterwards",
    "[av-media-source][retry][stop]") {
  TemporaryMedia missing;
  std::atomic<std::size_t> callbacks{0};
  AvMediaSource input(SourceConfig());
  input.SetOnStateChanged([&](const auto&) { ++callbacks; });
  REQUIRE(input.Start(missing.path()));
  REQUIRE(WaitFor([&] {
    return input.status().state == AvMediaSourceState::kWaitingRetry;
  }));
  const auto started = std::chrono::steady_clock::now();
  input.Stop();
  CHECK((std::chrono::steady_clock::now() - started < 500ms));
  CHECK((input.status().state == AvMediaSourceState::kStopped));
  const auto count = callbacks.load();
  std::this_thread::sleep_for(20ms);
  CHECK((callbacks.load() == count));
  CHECK((input.TryReadVideo().status == AvDecoderReadStatus::kNull));
  CHECK((input.TryReadAudio().status == AvDecoderReadStatus::kNull));
  input.Stop();
}

TEST_CASE(
    "AvMediaSource retains original asynchronous failures and publishes no "
    "fake "
    "EOF",
    "[av-media-source][error]") {
  bool missing = false;
  SECTION("unknown configured decoder") {}
  SECTION("opening fails with retries disabled") { missing = true; }
  TemporaryMedia temporary;
  auto config = SourceConfig();
  config.proxy.max_retries = 0;
  if (!missing) config.video_decoder.decoder_name = "mw_missing_decoder";
  std::exception_ptr observed;
  std::size_t failed_callbacks = 0;
  AvMediaSource input(config);
  input.SetOnStateChanged([&](const auto& status) {
    if (status.state == AvMediaSourceState::kFailed) {
      observed = status.error;
      ++failed_callbacks;
    }
  });
  REQUIRE(input.Start(missing ? temporary.path() : SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kFailed; }));
  input.Stop();
  CHECK((failed_callbacks == 1));
  CHECK((input.status().state == AvMediaSourceState::kFailed));
  CHECK((observed == input.status().error));
  REQUIRE((observed != nullptr));
  CHECK((input.TryReadVideo().status == AvDecoderReadStatus::kNull));
  CHECK((input.TryReadAudio().status == AvDecoderReadStatus::kNull));
  try {
    std::rethrow_exception(observed);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK((error.error_code() ==
           (missing ? AVERROR(ENOENT) : AVERROR_DECODER_NOT_FOUND)));
  }
}

TEST_CASE(
    "AvMediaSource serial observers can query, throw and request cancellation",
    "[av-media-source][notification]") {
  bool cancel = false;
  SECTION("observer exceptions do not stop data processing") {}
  SECTION("observer RequestStop is cooperative") { cancel = true; }
  std::mutex mutex;
  std::vector<AvMediaSourceState> events;
  std::thread::id worker;
  bool serial_thread = true;
  AvMediaSource input(SourceConfig());
  input.SetOnStateChanged([&](const auto& status) {
    static_cast<void>(input.status());
    static_cast<void>(input.info());
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (events.empty()) worker = std::this_thread::get_id();
      serial_thread &= worker == std::this_thread::get_id();
      events.push_back(status.state);
    }
    if (cancel && status.state == AvMediaSourceState::kRunning)
      input.RequestStop();
    throw std::runtime_error("observer failure");
  });
  REQUIRE(input.Start(SamplePath()));
  if (cancel) {
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvMediaSourceState::kStopped; }));
  } else {
    CheckSource(input, ReadNative(SamplePath()));
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  }
  input.Stop();
  std::lock_guard<std::mutex> lock(mutex);
  CHECK(serial_thread);
  CHECK((worker != std::this_thread::get_id()));
  REQUIRE_FALSE(events.empty());
  CHECK((events.front() == AvMediaSourceState::kConnecting));
  CHECK((events.back() ==
         (cancel ? AvMediaSourceState::kStopped : AvMediaSourceState::kEnded)));
}

TEST_CASE(
    "AvMediaSource returned frames and info survive Stop and a fresh Start",
    "[av-media-source][ownership][restart]") {
  const auto path = SamplePath("h264_video.mp4");
  AvMediaSource input(SourceConfig());
  REQUIRE(input.Start(path));
  mw::streamer::AvDecoderReadResult retained;
  REQUIRE(WaitFor([&] {
    retained = input.TryReadVideo();
    return retained.status == AvDecoderReadStatus::kFrame;
  }));
  REQUIRE(retained.frame.has_value());
  const auto before = Record(*retained.frame->get());
  const auto info = input.info();
  REQUIRE((info != nullptr));
  input.Stop();
  input.Stop();
  CHECK((input.TryReadVideo().status == AvDecoderReadStatus::kNull));
  CHECK((Record(*retained.frame->get()) == before));
  CHECK((info->video_stream_index == 0));
  REQUIRE(input.Start(path));
  CHECK_FALSE(input.Start(path));
  CheckSource(input, ReadNative(path));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  CHECK((Record(*retained.frame->get()) == before));
  CHECK((info->streams.front().codec_parameters.get()->codec_id ==
         AV_CODEC_ID_H264));
}

FrameRecord DownloadVideo(const ffmpeg::Frame& gpu) {
  ffmpeg::Frame downloaded;
  REQUIRE((av_hwframe_transfer_data(downloaded.get(), gpu.get(), 0) >= 0));
  REQUIRE((av_frame_copy_props(downloaded.get(), gpu.get()) >= 0));
  if (downloaded->format == AV_PIX_FMT_YUV420P)
    return Record(*downloaded.get());
  REQUIRE((downloaded->format == AV_PIX_FMT_NV12));
  ffmpeg::Frame planar;
  planar->format = AV_PIX_FMT_YUV420P;
  planar->width = downloaded->width;
  planar->height = downloaded->height;
  REQUIRE((av_frame_get_buffer(planar.get(), 0) >= 0));
  REQUIRE((av_frame_copy_props(planar.get(), downloaded.get()) >= 0));
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
    "AvMediaSource preserves decoded pixels, samples and source timestamps",
    "[av-media-source][oracle]") {
  AvMediaSource input(SourceConfig());
  REQUIRE(input.Start(SamplePath()));
  CheckSource(input, ReadNative(SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  input.Stop();
  CHECK((input.status().state == AvMediaSourceState::kEnded));
}

TEST_CASE("AvMediaSource loops retain frame contents and per-track EOF order",
          "[av-media-source][loop][eof]") {
  bool video_first = false;
  SECTION("both tracks are consumed together") {}
  SECTION("audio retains old frames while video completes two loops") {
    video_first = true;
  }
  const auto reference = ReadNative(SamplePath());
  auto config = SourceConfig(true);
  if (video_first) {
    REQUIRE(reference.audio.has_value());
    config.audio_decoder.frame_capacity =
        DecodeNative(*reference.audio).size() * 2 + 2;
  }
  AvMediaSource input(config);
  REQUIRE(input.Start(SamplePath()));
  CheckSource(input, reference, 2, video_first);
  CHECK((input.status().state != AvMediaSourceState::kEnded));
  input.Stop();
  CHECK((input.status().state == AvMediaSourceState::kStopped));
}

TEST_CASE("AvMediaSource bounded packet queues preserve compressed loop output",
          "[av-media-source][backpressure][loop][oracle]") {
  auto config = SourceConfig(true);
  config.video_decoder.packet_capacity = 1;
  config.audio_decoder.packet_capacity = 1;
  config.video_decoder.frame_capacity = 1;
  config.audio_decoder.frame_capacity = 1;
  AvMediaSource input(config);
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kRunning; }));
  std::this_thread::sleep_for(50ms);
  CheckSource(input, ReadNative(SamplePath()), 3);
  CHECK((input.status().state != AvMediaSourceState::kEnded));
  input.Stop();
}

TEST_CASE(
    "AvMediaSource retries partial EOF and Reset without duplicating input",
    "[av-media-source][backpressure][eof][reset][oracle]") {
  bool loop = false;
  SECTION("final EOF waits for both consumers") {}
  SECTION("loop reset follows both accepted EOFs") { loop = true; }
  TemporaryMedia temporary;
  temporary.CreateOrderedAv();
  const auto native = ReadNative(temporary.path());
  REQUIRE(native.video.has_value());
  REQUIRE(native.audio.has_value());
  const std::size_t cycles = loop ? 3 : 1;
  const std::array<std::vector<std::vector<FrameRecord>>, 2> expected{
      DecodeNativeCycles(*native.video, cycles),
      DecodeNativeCycles(*native.audio, cycles)};
  REQUIRE((expected[0][0].size() == 1));
  REQUIRE((expected[1][0].size() == 3));
  auto config = SourceConfig(loop);
  config.video_decoder.packet_capacity = 1;
  config.audio_decoder.packet_capacity = 1;
  config.video_decoder.frame_capacity = 1;
  config.audio_decoder.frame_capacity = 1;
  AvMediaSource input(config);
  REQUIRE(input.Start(temporary.path()));
  std::array<std::size_t, 2> eof_counts{};
  std::array<std::size_t, 2> frame_counts{};
  const auto consume = [&](std::size_t track) {
    REQUIRE((input.status().state != AvMediaSourceState::kFailed));
    const auto result =
        track == 0 ? input.TryReadVideo() : input.TryReadAudio();
    if (result.status == AvDecoderReadStatus::kNull) return false;
    REQUIRE((eof_counts[track] < cycles));
    const auto& frames = expected[track][eof_counts[track]];
    if (result.status == AvDecoderReadStatus::kEof) {
      CHECK((frame_counts[track] == frames.size()));
      CHECK_FALSE(result.frame.has_value());
      ++eof_counts[track];
      frame_counts[track] = 0;
    } else {
      REQUIRE((result.status == AvDecoderReadStatus::kFrame));
      REQUIRE(result.frame.has_value());
      REQUIRE((frame_counts[track] < frames.size()));
      CheckRecord(Record(*result.frame->get()), frames[frame_counts[track]++]);
    }
    return true;
  };
  REQUIRE(WaitFor([&] {
    consume(0);
    return eof_counts[0] == 1;
  }));
  const auto check_stalled_video = [&] {
    const auto until = std::chrono::steady_clock::now() + 30ms;
    do {
      CHECK_FALSE(consume(0));
      CHECK((input.status().state != AvMediaSourceState::kEnded));
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < until);
  };
  // The video's accepted EOF must not be retried while audio rejects its EOF.
  check_stalled_video();
  REQUIRE(WaitFor([&] { return consume(1); }));
  REQUIRE((frame_counts[1] == 1));
  // Freeing one audio frame lets its EOF enter the queue, but Reset is still
  // blocked behind it. A partial Reset must not release next-cycle video.
  check_stalled_video();
  REQUIRE(WaitFor([&] {
    for (std::size_t track = 0; track < 2; ++track) {
      if (eof_counts[track] < cycles) consume(track);
    }
    return eof_counts[0] == cycles && eof_counts[1] == cycles;
  }));
  if (loop) {
    CHECK((input.status().state != AvMediaSourceState::kEnded));
  } else {
    REQUIRE(WaitFor(
        [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  }
  input.Stop();
}

TEST_CASE("AvMediaSource validates arguments without introducing output modes",
          "[av-media-source][validation]") {
  AvMediaSource input;
  static_assert(noexcept(input.Start("")));
  CHECK_FALSE(input.Start(""));
  CHECK_FALSE(input.Start("  \t\n"));
  CHECK_FALSE(input.Start(std::string_view("a\0b", 3)));
  CHECK((input.status().state == AvMediaSourceState::kStopped));
  CHECK((input.status().error == nullptr));
  CHECK((input.TryReadVideo().status == AvDecoderReadStatus::kNull));
  CHECK((input.TryReadAudio().status == AvDecoderReadStatus::kNull));
  AvMediaSourceConfig config;
  SECTION("zero packet capacity") { config.proxy.input.packet_capacity = 0; }
  SECTION("zero video packet capacity") {
    config.video_decoder.packet_capacity = 0;
  }
  SECTION("zero audio packet capacity") {
    config.audio_decoder.packet_capacity = 0;
  }
  SECTION("zero video frame capacity") {
    config.video_decoder.frame_capacity = 0;
  }
  SECTION("negative audio codec threads") {
    config.audio_decoder.thread_count = -1;
  }
  SECTION("zero packet read timeout") { config.packet_read_timeout = 0ms; }
  SECTION("negative packet read timeout") { config.packet_read_timeout = -1ms; }
  CHECK_THROWS_AS(AvMediaSource(config), std::invalid_argument);
}

TEST_CASE("AvMediaSource stops under unread-track backpressure and restarts",
          "[av-media-source][backpressure][restart]") {
  auto config = SourceConfig(true);
  config.packet_read_timeout = 10s;
  config.video_decoder.packet_capacity = 1;
  config.audio_decoder.packet_capacity = 1;
  config.video_decoder.frame_capacity = 1;
  config.audio_decoder.frame_capacity = 1;
  AvMediaSource input(config);
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kRunning; }));
  // Leave both capacity-one output queues unread so decoder workers encounter
  // backpressure. Stop must cancel their queue waits and packet dispatch.
  std::this_thread::sleep_for(50ms);
  const auto before = std::chrono::steady_clock::now();
  input.Stop();
  CHECK((std::chrono::steady_clock::now() - before < 500ms));
  CHECK((input.status().state == AvMediaSourceState::kStopped));
  REQUIRE(input.Start(SamplePath()));
  CheckSource(input, ReadNative(SamplePath()), 2);
  input.Stop();
}

TEST_CASE("AvMediaSource supports independent concurrent readers during Stop",
          "[av-media-source][concurrency][stop]") {
  const auto native = ReadNative(SamplePath());
  REQUIRE(native.video.has_value());
  REQUIRE(native.audio.has_value());
  const std::array<std::vector<FrameRecord>, 2> expected{
      DecodeNative(*native.video), DecodeNative(*native.audio)};
  AvMediaSource input(SourceConfig(true));
  REQUIRE(input.Start(SamplePath()));
  std::atomic<bool> stop_readers{false};
  std::array<std::atomic<std::size_t>, 2> frames_read{};
  std::array<std::vector<mw::streamer::AvDecoderReadResult>, 2> results;
  std::array<std::exception_ptr, 2> errors;
  const auto read_track = [&](std::size_t index) {
    try {
      while (!stop_readers.load()) {
        auto result = index == 0 ? input.TryReadVideo() : input.TryReadAudio();
        if (result.status == AvDecoderReadStatus::kNull) {
          std::this_thread::sleep_for(100us);
          continue;
        }
        const bool frame = result.status == AvDecoderReadStatus::kFrame;
        results[index].push_back(std::move(result));
        if (frame) ++frames_read[index];
      }
    } catch (...) {
      errors[index] = std::current_exception();
    }
  };
  std::thread video_reader(read_track, 0);
  std::thread audio_reader(read_track, 1);
  const bool received = WaitFor([&] {
    return frames_read[0].load() >= expected[0].size() + 1 &&
           frames_read[1].load() >= expected[1].size() + 1;
  });
  // Readers remain active throughout Stop and are joined before assertions.
  const auto before = std::chrono::steady_clock::now();
  input.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  stop_readers = true;
  video_reader.join();
  audio_reader.join();
  REQUIRE(received);
  CHECK((elapsed < 500ms));
  CHECK((input.status().state == AvMediaSourceState::kStopped));
  for (std::size_t index = 0; index < results.size(); ++index) {
    CAPTURE(index);
    REQUIRE((errors[index] == nullptr));
    std::size_t count = 0;
    std::size_t eof_count = 0;
    const auto& track = index == 0 ? *native.video : *native.audio;
    for (const auto& result : results[index]) {
      CHECK((result.stream_index == track.stream.stream_index));
      CHECK((result.time_base.num == track.stream.time_base.num));
      CHECK((result.time_base.den == track.stream.time_base.den));
      if (result.status == AvDecoderReadStatus::kEof) {
        CHECK((count == expected[index].size()));
        ++eof_count;
        count = 0;
      } else {
        REQUIRE((result.status == AvDecoderReadStatus::kFrame));
        REQUIRE(result.frame.has_value());
        REQUIRE((count < expected[index].size()));
        CHECK((result.frame->get()->pts == expected[index][count++].pts));
      }
    }
    CHECK((eof_count >= 1));
  }
}

TEST_CASE(
    "AvMediaSource observes native packets once under decoder backpressure",
    "[av-media-source][packet]") {
  const auto native = ReadNative(SamplePath());
  auto config = SourceConfig();
  config.video_decoder.packet_capacity = 1;
  config.audio_decoder.packet_capacity = 1;
  config.video_decoder.frame_capacity = 1;
  config.audio_decoder.frame_capacity = 1;
  AvMediaSource input(config);
  std::vector<ffmpeg::Packet> packets;
  std::atomic<std::size_t> observed{0};
  input.SetOnPacket([&](const ffmpeg::Packet& packet) {
    static_cast<void>(input.status());
    static_cast<void>(input.info());
    packets.push_back(packet.Ref());
    ++observed;
  });
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return observed.load() >= 3; }));
  REQUIRE_THROWS_AS(input.SetOnPacket({}), std::logic_error);
  // With no frame consumer, both bounded decoder queues eventually stall.
  std::this_thread::sleep_for(30ms);
  CHECK((observed.load() < native.packets.size()));
  CheckSource(input, native);
  input.Stop();
  REQUIRE((packets.size() == native.packets.size()));
  for (std::size_t index = 0; index < packets.size(); ++index) {
    CAPTURE(index);
    const auto& packet = packets[index];
    const auto& expected = native.packets[index];
    CHECK((packet->stream_index == expected->stream_index));
    CHECK((packet->pts == expected->pts));
    CHECK((packet->dts == expected->dts));
    CHECK((packet->duration == expected->duration));
    CHECK((packet->flags == expected->flags));
    CHECK((packet->time_base.num == expected->time_base.num));
    CHECK((packet->time_base.den == expected->time_base.den));
    REQUIRE((packet->size == expected->size));
    REQUIRE((packet->size >= 0));
    CHECK((std::memcmp(packet->data, expected->data,
                       static_cast<std::size_t>(packet->size)) == 0));
  }
  const auto previous_count = observed.load();
  std::atomic<std::size_t> replacement_count{0};
  input.SetOnPacket([&](const ffmpeg::Packet&) { ++replacement_count; });
  REQUIRE(input.Start(SamplePath()));
  CheckSource(input, native);
  input.Stop();
  CHECK((observed.load() == previous_count));
  CHECK((replacement_count.load() == native.packets.size()));
}

TEST_CASE("AvMediaSource packet observer exceptions fail the source",
          "[av-media-source][packet]") {
  AvMediaSource input(SourceConfig());
  std::atomic<int> calls{0};
  std::atomic<bool> failed_wakeup{false};
  input.SetOnReadReady([&] {
    if (input.status().state == AvMediaSourceState::kFailed) {
      failed_wakeup = true;
    }
  });
  input.SetOnPacket([&](const ffmpeg::Packet&) {
    ++calls;
    throw std::logic_error("packet observer failed");
  });
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return failed_wakeup.load(); }));
  const auto failure = input.status().error;
  REQUIRE(failure);
  CHECK_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
  input.Stop();
  CHECK((calls.load() == 1));
  CHECK((input.status().state == AvMediaSourceState::kFailed));
}

TEST_CASE("AvMediaSource packet observers may request stop without joining",
          "[av-media-source][packet]") {
  AvMediaSource input(SourceConfig());
  std::atomic<int> calls{0};
  input.SetOnPacket([&](const ffmpeg::Packet&) {
    ++calls;
    input.RequestStop();
  });
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return calls.load() != 0; }));
  input.Stop();
  CHECK((calls.load() == 1));
  CHECK((input.status().state == AvMediaSourceState::kStopped));
}

TEST_CASE("AvMediaSource read wakeups support live replacement and exceptions",
          "[av-media-source][notification]") {
  std::atomic<int> old_calls{0};
  std::atomic<int> replacement_calls{0};
  std::atomic<int> state_calls{0};
  AvMediaSource input(SourceConfig());
  input.SetOnStateChanged([&](const auto&) { ++state_calls; });
  input.SetOnReadReady([&] { ++old_calls; });
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return old_calls.load() != 0; }));
  input.SetOnReadReady([&] {
    static_cast<void>(input.status());
    static_cast<void>(input.info());
    ++replacement_calls;
    throw std::runtime_error("read observer failure");
  });
  CheckSource(input, ReadNative(SamplePath()));
  REQUIRE(WaitFor(
      [&] { return input.status().state == AvMediaSourceState::kEnded; }));
  input.Stop();
  CHECK(input.status().error == nullptr);
  CHECK(replacement_calls.load() > 0);
  // Status notifications alone cannot satisfy this: both decoders must wake
  // the frame consumer after queuing decoded output and final EOF.
  CHECK(old_calls.load() + replacement_calls.load() > state_calls.load() + 2);
  input.SetOnReadReady({});
  const auto previous = replacement_calls.load();
  REQUIRE(input.Start(SamplePath()));
  CheckSource(input, ReadNative(SamplePath()));
  input.Stop();
  CHECK(replacement_calls.load() == previous);
}

TEST_CASE("AvMediaSource read observers may consume frames and request stop",
          "[av-media-source][notification]") {
  std::mutex reader_mutex;
  std::atomic<bool> received{false};
  AvMediaSource input(SourceConfig());
  input.SetOnReadReady([&] {
    std::lock_guard lock(reader_mutex);
    const auto video = input.TryReadVideo();
    const auto audio = input.TryReadAudio();
    if (video.status == AvDecoderReadStatus::kFrame ||
        audio.status == AvDecoderReadStatus::kFrame) {
      input.RequestStop();
      received = true;
    }
  });
  REQUIRE(input.Start(SamplePath()));
  REQUIRE(WaitFor([&] { return received.load(); }));
  input.Stop();
  CHECK(input.status().state == AvMediaSourceState::kStopped);
  CHECK(input.status().error == nullptr);
}

TEST_CASE("AvMediaSource CUDA frames share and retain the supplied device",
          "[.][av-media-source][cuda]") {
  std::optional<ffmpeg::Frame> retained;
  std::optional<FrameRecord> retained_record;
  {
    ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
    const auto native = ReadNative(SamplePath());
    REQUIRE(native.video.has_value());
    const auto expected = DecodeNative(*native.video);
    auto config = SourceConfig();
    AvMediaSource input(device, config);
    REQUIRE(input.Start(SamplePath()));
    std::size_t count = 0;
    bool video_eof = false;
    bool audio_eof = false;
    const bool ready = WaitFor([&] {
      REQUIRE((input.status().state != AvMediaSourceState::kFailed));
      if (!audio_eof) {
        const auto audio = input.TryReadAudio();
        audio_eof = audio.status == AvDecoderReadStatus::kEof;
        if (audio.frame) CHECK((audio.frame->get()->hw_frames_ctx == nullptr));
      }
      if (!video_eof) {
        auto video = input.TryReadVideo();
        video_eof = video.status == AvDecoderReadStatus::kEof;
        if (video.frame) {
          REQUIRE((video.frame->get()->format == AV_PIX_FMT_CUDA));
          REQUIRE((video.frame->get()->hw_frames_ctx != nullptr));
          const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
              video.frame->get()->hw_frames_ctx->data);
          REQUIRE((frames->device_ref != nullptr));
          CHECK((frames->device_ref->data == device.get()->data));
          REQUIRE((count < expected.size()));
          const auto actual = DownloadVideo(*video.frame);
          CheckRecord(actual, expected[count++]);
          if (!retained) {
            retained = video.frame->Ref();
            retained_record = actual;
          }
        }
      }
      return video_eof && audio_eof;
    });
    REQUIRE(ready);
    CHECK((count == expected.size()));
    input.Stop();
  }
  REQUIRE(retained.has_value());
  REQUIRE(retained_record.has_value());
  CHECK((DownloadVideo(*retained) == *retained_record));
}

}  // namespace

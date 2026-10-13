#include "mw/streamer/decoder/av_decoder.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
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
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/error.h"

namespace {

using namespace std::chrono_literals;
using mw::streamer::AvDecoder;
using mw::streamer::AvDecoderConfig;
using mw::streamer::AvDecoderReadResult;
using mw::streamer::AvDecoderReadStatus;
namespace ffmpeg = mw::streamer::ffmpeg;

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

struct Source {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;
};

Source ReadSource(std::string_view file, AVMediaType type) {
  const auto path = fmt::format("{}/{}", MW_AV_DECODER_TEST_DATA_DIR, file);
  AVFormatContext* context = nullptr;
  const int opened =
      avformat_open_input(&context, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(context);
  REQUIRE(opened >= 0);
  REQUIRE(avformat_find_stream_info(context, nullptr) >= 0);
  const int index = av_find_best_stream(context, type, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  const auto* stream = context->streams[index];
  Source source;
  source.stream = {index, ffmpeg::CodecParameters(*stream->codecpar),
                   stream->time_base};
  source.stream.frame_rate =
      av_guess_frame_rate(context, context->streams[index], nullptr);
  source.stream.sample_aspect_ratio =
      av_guess_sample_aspect_ratio(context, context->streams[index], nullptr);
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(context, packet.get());
    if (result == AVERROR_EOF) break;
    REQUIRE(result >= 0);
    if (packet->stream_index == index) {
      source.packets.push_back(std::move(packet));
    }
  }
  REQUIRE_FALSE(source.packets.empty());
  return source;
}

struct FrameRecord {
  std::int64_t pts = AV_NOPTS_VALUE;
  int format = -1;
  int width = 0;
  int height = 0;
  int samples = 0;
  int sample_rate = 0;
  int channels = 0;
  std::vector<std::uint8_t> content;

  bool operator==(const FrameRecord& other) const {
    return pts == other.pts && format == other.format && width == other.width &&
           height == other.height && samples == other.samples &&
           sample_rate == other.sample_rate && channels == other.channels &&
           content == other.content;
  }
};

// Compare media bytes only: allocator alignment/padding is not decoded content.
FrameRecord Record(const AVFrame& frame, std::int64_t pts) {
  FrameRecord result;
  result.pts = pts;
  result.format = frame.format;
  result.width = frame.width;
  result.height = frame.height;
  result.samples = frame.nb_samples;
  result.sample_rate = frame.sample_rate;
  result.channels = frame.ch_layout.nb_channels;
  if (frame.nb_samples > 0) {
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? frame.ch_layout.nb_channels : 1;
    const int bytes = frame.nb_samples * av_get_bytes_per_sample(format) *
                      (planar ? 1 : frame.ch_layout.nb_channels);
    REQUIRE(bytes > 0);
    for (int plane = 0; plane < planes; ++plane) {
      result.content.insert(result.content.end(), frame.extended_data[plane],
                            frame.extended_data[plane] + bytes);
    }
  } else {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const int size =
        av_image_get_buffer_size(format, frame.width, frame.height, 1);
    REQUIRE(size > 0);
    result.content.resize(static_cast<std::size_t>(size));
    REQUIRE(av_image_copy_to_buffer(result.content.data(), size, frame.data,
                                    frame.linesize, format, frame.width,
                                    frame.height, 1) == size);
  }
  return result;
}

std::int64_t FrameEnd(const AVFrame& frame, const ffmpeg::StreamInfo& stream) {
  if (frame.best_effort_timestamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
  std::int64_t duration = frame.duration;
  if (duration <= 0 && frame.nb_samples > 0 && frame.sample_rate > 0) {
    duration = av_rescale_q(frame.nb_samples, AVRational{1, frame.sample_rate},
                            stream.time_base);
  } else if (duration <= 0 && stream.frame_rate.num > 0 &&
             stream.frame_rate.den > 0) {
    duration = av_rescale_q(1, av_inv_q(stream.frame_rate), stream.time_base);
  }
  return duration > 0 ? frame.best_effort_timestamp + duration : AV_NOPTS_VALUE;
}

struct Reference {
  std::vector<FrameRecord> frames;
  std::size_t before_eof = 0;
  std::int64_t end_pts = AV_NOPTS_VALUE;
};

std::unique_ptr<AVCodecContext, CodecDeleter> OpenReferenceCodec(
    const ffmpeg::StreamInfo& stream) {
  const auto* codec =
      avcodec_find_decoder(stream.codec_parameters.get()->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecDeleter> context(
      avcodec_alloc_context3(codec));
  REQUIRE(context != nullptr);
  REQUIRE(avcodec_parameters_to_context(context.get(),
                                        stream.codec_parameters.get()) >= 0);
  context->pkt_timebase = stream.time_base;
  context->framerate = stream.frame_rate;
  context->sample_aspect_ratio = stream.sample_aspect_ratio;
  context->thread_count = 1;
  REQUIRE(avcodec_open2(context.get(), codec, nullptr) >= 0);
  return context;
}

// Raw send/receive supplies an oracle independent of AvDecoder and its queues.
Reference ReferenceDecode(const ffmpeg::StreamInfo& stream,
                          const std::vector<ffmpeg::Packet>& packets,
                          bool drain = true) {
  auto context = OpenReferenceCodec(stream);
  Reference result;
  ffmpeg::Frame frame;
  const auto receive = [&] {
    for (;;) {
      const int code = avcodec_receive_frame(context.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      result.frames.push_back(
          Record(*frame.get(), frame->best_effort_timestamp));
      result.end_pts = FrameEnd(*frame.get(), stream);
    }
  };
  for (const auto& packet : packets) {
    REQUIRE(avcodec_send_packet(context.get(), packet.get()) >= 0);
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  result.before_eof = result.frames.size();
  if (drain) {
    REQUIRE(avcodec_send_packet(context.get(), nullptr) >= 0);
    REQUIRE(receive() == AVERROR_EOF);
  }
  return result;
}

template <typename Predicate>
bool WaitFor(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

AvDecoderReadResult ReadAvailable(AvDecoder& decoder) {
  AvDecoderReadResult result;
  REQUIRE(WaitFor([&] {
    result = decoder.TryReadFrame();
    return result.status != AvDecoderReadStatus::kNull || decoder.error();
  }));
  REQUIRE(decoder.error() == nullptr);
  return result;
}

void PushGeneration(AvDecoder& decoder, const Source& source,
                    std::uint64_t generation) {
  for (const auto& packet : source.packets) {
    REQUIRE(decoder.PushPacket(packet.Ref(), generation));
  }
  REQUIRE(decoder.PushEof(generation));
}

void CheckGeneration(AvDecoder& decoder, const Source& source,
                     const Reference& reference, std::uint64_t generation) {
  for (const auto& expected : reference.frames) {
    auto result = ReadAvailable(decoder);
    REQUIRE(result.status == AvDecoderReadStatus::kFrame);
    REQUIRE(result.frame.has_value());
    CHECK(result.generation == generation);
    CHECK(result.stream_index == source.stream.stream_index);
    CHECK(result.time_base.num == source.stream.time_base.num);
    CHECK(result.time_base.den == source.stream.time_base.den);
    CHECK(Record(*result.frame->get(), result.frame->get()->pts) == expected);
  }
  const auto eof = ReadAvailable(decoder);
  CHECK(eof.status == AvDecoderReadStatus::kEof);
  CHECK_FALSE(eof.frame.has_value());
  CHECK(eof.generation == generation);
  CHECK(eof.stream_index == source.stream.stream_index);
  CHECK(eof.time_base.num == source.stream.time_base.num);
  CHECK(eof.time_base.den == source.stream.time_base.den);
  CHECK(eof.end_pts == reference.end_pts);
}

TEST_CASE("AvDecoder matches native video pixels, audio samples and source PTS",
          "[av-decoder][decode]") {
  for (const auto file :
       {"h264_aac.mp4", "h265_aac.mp4", "vp8_video.ivf", "vp9_video.ivf"}) {
    for (const auto type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
      if (type == AVMEDIA_TYPE_AUDIO &&
          std::string_view(file).find(".ivf") != std::string_view::npos) {
        continue;
      }
      CAPTURE(file, type);
      const auto source = ReadSource(file, type);
      const auto reference = ReferenceDecode(source.stream, source.packets);
      REQUIRE_FALSE(reference.frames.empty());
      AvDecoderConfig config;
      config.thread_count = 1;
      config.frame_capacity = 2;
      AvDecoder decoder(source.stream, config);
      PushGeneration(decoder, source, 5);
      CheckGeneration(decoder, source, reference, 5);
      CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
    }
  }
}

TEST_CASE("AvDecoder drains delayed B frames before its separate EOF item",
          "[av-decoder][eof]") {
  const auto source = ReadSource("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto reference = ReferenceDecode(source.stream, source.packets);
  REQUIRE(reference.before_eof < reference.frames.size());
  AvDecoderConfig config;
  SECTION("single codec thread") { config.thread_count = 1; }
  SECTION("automatic codec threading") { config.thread_count = 0; }
  config.frame_capacity = 1;
  AvDecoder decoder(source.stream, config);
  PushGeneration(decoder, source, 0);
  CheckGeneration(decoder, source, reference, 0);
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("AvDecoder queues complete generations and EOF in FIFO order",
          "[av-decoder][generation][eof]") {
  const auto source = ReadSource("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto reference = ReferenceDecode(source.stream, source.packets);
  AvDecoderConfig config;
  config.thread_count = 1;
  config.frame_capacity = 2;
  AvDecoder decoder(source.stream, config);
  PushGeneration(decoder, source, 10);
  PushGeneration(decoder, source, 13);
  CheckGeneration(decoder, source, reference, 10);
  CheckGeneration(decoder, source, reference, 13);
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE(
    "AvDecoder generation change drops codec delay and keeps queued frames",
    "[av-decoder][generation][flush]") {
  const auto source = ReadSource("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto half = source.packets.size() / 2;
  std::vector<ffmpeg::Packet> prefix(
      source.packets.begin(),
      source.packets.begin() + static_cast<std::ptrdiff_t>(half));
  const auto partial = ReferenceDecode(source.stream, prefix, false);
  const auto complete = ReferenceDecode(source.stream, source.packets);
  REQUIRE_FALSE(partial.frames.empty());
  REQUIRE(partial.frames.size() < prefix.size());
  AvDecoderConfig config;
  config.thread_count = 1;
  config.frame_capacity = partial.frames.size() + complete.frames.size() + 1;
  AvDecoder decoder(source.stream, config);
  for (const auto& packet : prefix) REQUIRE(decoder.PushPacket(packet, 2));
  REQUIRE(WaitFor([&] {
    return decoder.buffered_frames() == partial.frames.size() &&
           decoder.buffered_packets() == 0;
  }));
  PushGeneration(decoder, source, 3);
  for (const auto& expected : partial.frames) {
    const auto result = ReadAvailable(decoder);
    REQUIRE(result.status == AvDecoderReadStatus::kFrame);
    REQUIRE(result.frame.has_value());
    CHECK(result.generation == 2);
    CHECK(Record(*result.frame->get(), result.frame->get()->pts) == expected);
  }
  CheckGeneration(decoder, source, complete, 3);
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE(
    "AvDecoder output capacity applies backpressure and Stop preserves frames",
    "[av-decoder][backpressure][stop][ownership]") {
  const auto source = ReadSource("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto reference = ReferenceDecode(source.stream, source.packets);
  AvDecoderConfig config;
  config.thread_count = 1;
  config.frame_capacity = 1;
  AvDecoder decoder(source.stream, config);
  PushGeneration(decoder, source, 1);
  REQUIRE(WaitFor([&] {
    return decoder.buffered_frames() == 1 &&
           decoder.buffered_packets() > config.packet_watermark;
  }));
  CHECK_FALSE(decoder.wants_packet());
  std::this_thread::sleep_for(20ms);
  CHECK(decoder.buffered_frames() == config.frame_capacity);
  auto retained = ReadAvailable(decoder);
  REQUIRE(retained.status == AvDecoderReadStatus::kFrame);
  REQUIRE(retained.frame.has_value());
  const auto before =
      Record(*retained.frame->get(), retained.frame->get()->pts);
  CHECK(before == reference.frames.front());
  REQUIRE(WaitFor([&] { return decoder.buffered_frames() == 1; }));
  const auto started = std::chrono::steady_clock::now();
  decoder.Stop();
  CHECK(std::chrono::steady_clock::now() - started < 500ms);
  CHECK(decoder.buffered_frames() == 0);
  CHECK(decoder.buffered_packets() == 0);
  CHECK_FALSE(decoder.wants_packet());
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
  CHECK_FALSE(decoder.PushPacket(source.packets.front(), 2));
  CHECK_FALSE(decoder.PushEof(2));
  CHECK(Record(*retained.frame->get(), retained.frame->get()->pts) == before);
}

TEST_CASE("AvDecoder empty reads never wait and Stop wakes its idle worker",
          "[av-decoder][nonblocking][stop]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_AUDIO);
  AvDecoder decoder(source.stream);
  const auto read_start = std::chrono::steady_clock::now();
  const auto result = decoder.TryReadFrame();
  CHECK(std::chrono::steady_clock::now() - read_start < 100ms);
  CHECK(result.status == AvDecoderReadStatus::kNull);
  CHECK_FALSE(result.frame.has_value());
  CHECK(decoder.wants_packet());
  CHECK(decoder.error() == nullptr);
  decoder.RequestStop();
  const auto stop_start = std::chrono::steady_clock::now();
  decoder.Stop();
  CHECK(std::chrono::steady_clock::now() - stop_start < 500ms);
  decoder.Stop();
}

TEST_CASE("AvDecoder rejects invalid queue and codec configuration",
          "[av-decoder][validation]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
  AvDecoderConfig config;
  SECTION("zero packet watermark") { config.packet_watermark = 0; }
  SECTION("zero frame capacity") { config.frame_capacity = 0; }
  SECTION("negative codec threads") { config.thread_count = -1; }
  CHECK_THROWS_AS(AvDecoder(source.stream, config), std::invalid_argument);
}

TEST_CASE(
    "AvDecoder rejects invalid packets and repeated or stale EOF generations",
    "[av-decoder][validation][generation]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
  AvDecoder decoder(source.stream);
  auto wrong_track = source.packets.front().Ref();
  ++wrong_track->stream_index;
  CHECK_THROWS_AS(decoder.PushPacket(std::move(wrong_track), 0),
                  std::invalid_argument);
  CHECK_THROWS_AS(decoder.PushPacket(ffmpeg::Packet(), 0),
                  std::invalid_argument);
  REQUIRE(decoder.PushPacket(source.packets.front(), 42));
  REQUIRE(decoder.PushEof(42));
  CHECK_THROWS_AS(decoder.PushEof(42), std::logic_error);
  CHECK_THROWS_AS(decoder.PushPacket(source.packets.front(), 42),
                  std::logic_error);
  CHECK_THROWS_AS(decoder.PushPacket(source.packets.front(), 41),
                  std::logic_error);
  CHECK_THROWS_AS(decoder.PushEof(41), std::logic_error);
  REQUIRE(decoder.PushPacket(source.packets.front(), 44));
  REQUIRE(decoder.PushEof(44));
}

TEST_CASE(
    "AvDecoder reports native decode failures once without publishing EOF",
    "[av-decoder][error][notification]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto* parameters = source.stream.codec_parameters.get();
  REQUIRE(parameters->extradata_size >= 5);
  REQUIRE(parameters->extradata[0] == 1);
  REQUIRE((parameters->extradata[4] & 3) + 1 > 1);
  ffmpeg::Packet invalid;
  REQUIRE(av_new_packet(invalid.get(), 1) >= 0);
  invalid->data[0] = 0;
  invalid->stream_index = source.stream.stream_index;
  invalid->pts = 0;
  invalid->dts = 0;
  auto native = OpenReferenceCodec(source.stream);
  const int native_error = avcodec_send_packet(native.get(), invalid.get());
  REQUIRE(native_error == AVERROR_INVALIDDATA);

  std::atomic<std::size_t> callbacks{0};
  std::mutex mutex;
  std::exception_ptr observed;
  bool callback_error_matches = false;
  AvDecoderReadStatus callback_read_status = AvDecoderReadStatus::kEof;
  AvDecoderConfig config;
  config.thread_count = 1;
  AvDecoder decoder(source.stream, config, [&](std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      observed = error;
      callback_error_matches = decoder.error() == error;
      callback_read_status = decoder.TryReadFrame().status;
    }
    decoder.RequestStop();
    ++callbacks;
    throw std::runtime_error("observer failure");
  });
  REQUIRE(decoder.PushPacket(invalid, 1));
  REQUIRE(WaitFor([&] { return callbacks.load() != 0; }));
  decoder.Stop();
  CHECK(callbacks.load() == 1);
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
  CHECK_FALSE(decoder.PushPacket(source.packets.front(), 2));
  CHECK_FALSE(decoder.PushEof(2));
  std::lock_guard<std::mutex> lock(mutex);
  CHECK(callback_error_matches);
  CHECK(callback_read_status == AvDecoderReadStatus::kNull);
  CHECK(observed == decoder.error());
  REQUIRE(observed != nullptr);
  try {
    std::rethrow_exception(observed);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == native_error);
  }
}

}  // namespace

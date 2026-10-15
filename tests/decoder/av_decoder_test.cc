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
Reference ReferenceDecode(AVCodecContext* context,
                          const ffmpeg::StreamInfo& stream,
                          const std::vector<ffmpeg::Packet>& packets,
                          bool drain = true) {
  Reference result;
  ffmpeg::Frame frame;
  const auto receive = [&] {
    for (;;) {
      const int code = avcodec_receive_frame(context, frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      result.frames.push_back(
          Record(*frame.get(), frame->best_effort_timestamp));
      result.end_pts = FrameEnd(*frame.get(), stream);
    }
  };
  for (const auto& packet : packets) {
    REQUIRE(avcodec_send_packet(context, packet.get()) >= 0);
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  result.before_eof = result.frames.size();
  if (drain) {
    REQUIRE(avcodec_send_packet(context, nullptr) >= 0);
    REQUIRE(receive() == AVERROR_EOF);
  }
  return result;
}

Reference ReferenceDecode(const ffmpeg::StreamInfo& stream,
                          const std::vector<ffmpeg::Packet>& packets,
                          bool drain = true) {
  auto context = OpenReferenceCodec(stream);
  return ReferenceDecode(context.get(), stream, packets, drain);
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

// The producer retries the same item while the test consumes bounded output.
// Unwinding stops the decoder before joining, so failed assertions cannot hang.
class SourceFeeder {
 public:
  SourceFeeder(AvDecoder& decoder, const Source& source, int replays = 1)
      : decoder_(decoder), worker_([this, &source, replays] {
          try {
            const auto submit = [&](auto push) {
              const auto deadline = std::chrono::steady_clock::now() + 5s;
              while (!push()) {
                if (decoder_.error() || stopping_.load() ||
                    std::chrono::steady_clock::now() >= deadline) {
                  throw std::runtime_error(
                      "Decoder input did not accept retry");
                }
                std::this_thread::sleep_for(1ms);
              }
            };
            for (int replay = 0; replay < replays; ++replay) {
              if (replay) submit([&] { return decoder_.TryReset(); });
              for (const auto& packet : source.packets) {
                submit([&] { return decoder_.TryPushPacket(packet); });
              }
              submit([&] { return decoder_.TryPushEof(); });
            }
          } catch (...) {
            failure_ = std::current_exception();
          }
        }) {}

  ~SourceFeeder() {
    if (worker_.joinable()) {
      stopping_.store(true);
      decoder_.RequestStop();
      worker_.join();
    }
  }

  void Join() {
    worker_.join();
    if (failure_) std::rethrow_exception(failure_);
  }

 private:
  AvDecoder& decoder_;
  std::atomic<bool> stopping_{false};
  std::exception_ptr failure_;
  std::thread worker_;
};

void CheckSource(AvDecoder& decoder, const Source& source,
                 const Reference& reference) {
  for (const auto& expected : reference.frames) {
    auto result = ReadAvailable(decoder);
    REQUIRE(result.status == AvDecoderReadStatus::kFrame);
    REQUIRE(result.frame.has_value());
    CHECK(result.stream_index == source.stream.stream_index);
    CHECK(result.time_base.num == source.stream.time_base.num);
    CHECK(result.time_base.den == source.stream.time_base.den);
    CHECK(Record(*result.frame->get(), result.frame->get()->pts) == expected);
  }
  const auto eof = ReadAvailable(decoder);
  CHECK(eof.status == AvDecoderReadStatus::kEof);
  CHECK_FALSE(eof.frame.has_value());
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
      SourceFeeder feeder(decoder, source);
      CheckSource(decoder, source, reference);
      feeder.Join();
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
  SourceFeeder feeder(decoder, source);
  CheckSource(decoder, source, reference);
  feeder.Join();
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE("AvDecoder queues reset and complete replays in FIFO order",
          "[av-decoder][reset][eof]") {
  const auto source = ReadSource("h265_aac.mp4", AVMEDIA_TYPE_VIDEO);
  const auto reference = ReferenceDecode(source.stream, source.packets);
  AvDecoderConfig config;
  config.thread_count = 1;
  config.frame_capacity = 2;
  AvDecoder decoder(source.stream, config);
  SourceFeeder feeder(decoder, source, 2);
  CheckSource(decoder, source, reference);
  CheckSource(decoder, source, reference);
  feeder.Join();
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
}

TEST_CASE(
    "AvDecoder reset without EOF drops codec delay and keeps queued frames",
    "[av-decoder][reset][flush]") {
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
  for (const auto& packet : prefix) {
    REQUIRE(WaitFor([&] { return decoder.TryPushPacket(packet); }));
  }
  REQUIRE(WaitFor([&] {
    return decoder.buffered_frames() == partial.frames.size() &&
           decoder.buffered_packets() == 0;
  }));
  REQUIRE(decoder.TryReset());
  SourceFeeder feeder(decoder, source);
  for (const auto& expected : partial.frames) {
    const auto result = ReadAvailable(decoder);
    REQUIRE(result.status == AvDecoderReadStatus::kFrame);
    REQUIRE(result.frame.has_value());
    CHECK(Record(*result.frame->get(), result.frame->get()->pts) == expected);
  }
  CheckSource(decoder, source, complete);
  feeder.Join();
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
  std::size_t submitted = 0;
  REQUIRE(WaitFor([&] {
    while (submitted < source.packets.size() &&
           decoder.TryPushPacket(source.packets[submitted])) {
      ++submitted;
    }
    return decoder.buffered_frames() == 1 &&
           decoder.buffered_packets() == config.packet_capacity;
  }));
  REQUIRE(submitted < source.packets.size());
  const auto& pending = source.packets[submitted];
  const auto* packet_address = pending.get();
  const auto* packet_data = pending->data;
  const auto packet_pts = pending->pts;
  const auto packet_size = pending->size;
  const auto started_push = std::chrono::steady_clock::now();
  CHECK_FALSE(decoder.TryPushPacket(pending));
  CHECK_FALSE(decoder.TryPushEof());
  CHECK_FALSE(decoder.TryReset());
  CHECK(std::chrono::steady_clock::now() - started_push < 100ms);
  CHECK(pending.get() == packet_address);
  CHECK(pending->data == packet_data);
  CHECK(pending->pts == packet_pts);
  CHECK(pending->size == packet_size);
  CHECK(decoder.buffered_packets() == config.packet_capacity);
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
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
  CHECK_FALSE(decoder.TryPushPacket(source.packets.front()));
  CHECK_FALSE(decoder.TryPushEof());
  CHECK_FALSE(decoder.TryReset());
  CHECK(Record(*retained.frame->get(), retained.frame->get()->pts) == before);
}

TEST_CASE("AvDecoder retries bounded EOF and Reset without losing FIFO order",
          "[av-decoder][backpressure][reset][eof]") {
  Source source;
  source.stream.stream_index = 0;
  source.stream.time_base = {1, 25};
  source.stream.frame_rate = {25, 1};
  auto* parameters = source.stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
  parameters->format = AV_PIX_FMT_GRAY8;
  parameters->width = 2;
  parameters->height = 2;
  for (int index = 0; index < 6; ++index) {
    ffmpeg::Packet packet;
    REQUIRE(av_new_packet(packet.get(), 4) >= 0);
    packet->stream_index = source.stream.stream_index;
    packet->pts = index;
    packet->dts = index;
    packet->duration = 1;
    for (int pixel = 0; pixel < packet->size; ++pixel) {
      packet->data[pixel] = static_cast<std::uint8_t>(index * 4 + pixel);
    }
    source.packets.push_back(std::move(packet));
  }
  const auto reference = ReferenceDecode(source.stream, source.packets);
  REQUIRE(reference.before_eof == source.packets.size());
  REQUIRE(reference.frames.size() == source.packets.size());
  AvDecoderConfig config;
  config.packet_capacity = 1;
  config.frame_capacity = 1;
  config.thread_count = 1;
  AvDecoder decoder(source.stream, config);
  // With no reads and exactly one frame per packet, accepting three packets
  // guarantees one queued frame, one frame blocked in Emit and one queued
  // packet.
  constexpr std::size_t submitted = 3;
  for (std::size_t index = 0; index < submitted; ++index) {
    REQUIRE(
        WaitFor([&] { return decoder.TryPushPacket(source.packets[index]); }));
  }
  REQUIRE(WaitFor([&] {
    return decoder.buffered_frames() == 1 && decoder.buffered_packets() == 1;
  }));
  CHECK_FALSE(decoder.TryPushEof());
  CHECK_FALSE(decoder.TryReset());
  // Neither rejected control item changed EOF acceptance state.
  CHECK_FALSE(decoder.TryPushPacket(source.packets[submitted]));

  std::vector<AvDecoderReadResult> output;
  output.push_back(ReadAvailable(decoder));
  REQUIRE(WaitFor([&] {
    return decoder.buffered_frames() == 1 && decoder.buffered_packets() == 0;
  }));
  // One decoded frame is blocked behind the full output queue. EOF fills the
  // sole input slot; Reset must retry without clearing accepted EOF state.
  REQUIRE(decoder.TryPushEof());
  CHECK_FALSE(decoder.TryReset());
  CHECK_THROWS_AS(decoder.TryPushPacket(source.packets[submitted]),
                  std::logic_error);
  CHECK_THROWS_AS(decoder.TryPushEof(), std::logic_error);
  const auto submit = [&](auto push) {
    REQUIRE(WaitFor([&] {
      REQUIRE(decoder.error() == nullptr);
      if (push()) return true;
      auto result = decoder.TryReadFrame();
      if (result.status != AvDecoderReadStatus::kNull) {
        output.push_back(std::move(result));
      }
      CHECK(decoder.buffered_packets() <= config.packet_capacity);
      return false;
    }));
  };
  submit([&] { return decoder.TryReset(); });
  for (const auto& packet : source.packets) {
    submit([&] { return decoder.TryPushPacket(packet); });
  }
  submit([&] { return decoder.TryPushEof(); });
  std::size_t eof_count = 0;
  for (const auto& result : output) {
    eof_count += result.status == AvDecoderReadStatus::kEof;
  }
  while (eof_count < 2) {
    auto result = ReadAvailable(decoder);
    eof_count += result.status == AvDecoderReadStatus::kEof;
    output.push_back(std::move(result));
  }
  const std::vector<ffmpeg::Packet> prefix(
      source.packets.begin(),
      source.packets.begin() + static_cast<std::ptrdiff_t>(submitted));
  auto native = OpenReferenceCodec(source.stream);
  const auto first = ReferenceDecode(native.get(), source.stream, prefix);
  avcodec_flush_buffers(native.get());
  const auto second =
      ReferenceDecode(native.get(), source.stream, source.packets);
  std::size_t index = 0;
  for (const auto* expected : {&first, &second}) {
    for (const auto& frame : expected->frames) {
      REQUIRE(index < output.size());
      const auto& actual = output[index++];
      REQUIRE(actual.status == AvDecoderReadStatus::kFrame);
      REQUIRE(actual.frame.has_value());
      CHECK(Record(*actual.frame->get(), actual.frame->get()->pts) == frame);
    }
    REQUIRE(index < output.size());
    CHECK(output[index].status == AvDecoderReadStatus::kEof);
    CHECK(output[index++].end_pts == expected->end_pts);
  }
  CHECK(index == output.size());
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
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
  CHECK(decoder.buffered_packets() == 0);
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
  SECTION("zero packet capacity") { config.packet_capacity = 0; }
  SECTION("zero frame capacity") { config.frame_capacity = 0; }
  SECTION("negative codec threads") { config.thread_count = -1; }
  CHECK_THROWS_AS(AvDecoder(source.stream, config), std::invalid_argument);
}

TEST_CASE("AvDecoder rejects input after EOF until an ordered reset",
          "[av-decoder][validation][reset]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_VIDEO);
  AvDecoder decoder(source.stream);
  auto wrong_track = source.packets.front().Ref();
  ++wrong_track->stream_index;
  CHECK_THROWS_AS(decoder.TryPushPacket(std::move(wrong_track)),
                  std::invalid_argument);
  CHECK_THROWS_AS(decoder.TryPushPacket(ffmpeg::Packet()),
                  std::invalid_argument);
  REQUIRE(decoder.TryPushPacket(source.packets.front()));
  REQUIRE(decoder.TryPushEof());
  CHECK_THROWS_AS(decoder.TryPushEof(), std::logic_error);
  CHECK_THROWS_AS(decoder.TryPushPacket(source.packets.front()),
                  std::logic_error);
  REQUIRE(decoder.TryReset());
  REQUIRE(decoder.TryPushPacket(source.packets.front()));
  REQUIRE(decoder.TryPushEof());
}

TEST_CASE("AvDecoder reset clears the previous EOF endpoint without media",
          "[av-decoder][reset][eof]") {
  const auto source = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_AUDIO);
  auto native = OpenReferenceCodec(source.stream);
  const auto reference =
      ReferenceDecode(native.get(), source.stream, source.packets);
  // A flushed AAC context need not reproduce a fresh context's exact samples.
  // Mirror the ordered flush/empty drain/flush sequence in the native oracle.
  avcodec_flush_buffers(native.get());
  const auto empty = ReferenceDecode(native.get(), source.stream, {});
  CHECK(empty.frames.empty());
  CHECK(empty.end_pts == AV_NOPTS_VALUE);
  avcodec_flush_buffers(native.get());
  const auto replay =
      ReferenceDecode(native.get(), source.stream, source.packets);
  AvDecoderConfig config;
  config.thread_count = 1;
  AvDecoder decoder(source.stream, config);
  SourceFeeder feeder(decoder, source);
  CheckSource(decoder, source, reference);
  feeder.Join();
  REQUIRE(decoder.TryReset());
  REQUIRE(decoder.TryPushEof());
  const auto eof = ReadAvailable(decoder);
  CHECK(eof.status == AvDecoderReadStatus::kEof);
  CHECK(eof.end_pts == AV_NOPTS_VALUE);
  REQUIRE(decoder.TryReset());
  SourceFeeder replay_feeder(decoder, source);
  CheckSource(decoder, source, replay);
  replay_feeder.Join();
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
  std::atomic<std::size_t> replacement_callbacks{0};
  std::mutex mutex;
  std::exception_ptr observed;
  bool callback_error_matches = false;
  AvDecoderReadStatus callback_read_status = AvDecoderReadStatus::kEof;
  AvDecoderConfig config;
  config.thread_count = 1;
  AvDecoder decoder(source.stream, config);
  decoder.SetOnError([&](std::exception_ptr error) {
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
  bool replaced = false;
  bool cleared = false;
  SECTION("throwing observer can query the failure and request stop") {}
  SECTION("replacement invokes only the new observer") {
    replaced = true;
    decoder.SetOnError([&](std::exception_ptr error) {
      observed = error;
      ++replacement_callbacks;
    });
  }
  SECTION("cleared observer leaves the original failure queryable") {
    cleared = true;
    decoder.SetOnError({});
  }
  REQUIRE(decoder.TryPushPacket(invalid));
  REQUIRE(WaitFor([&] {
    return cleared ? decoder.error() != nullptr
                   : callbacks.load() + replacement_callbacks.load() != 0;
  }));
  decoder.Stop();
  CHECK(callbacks.load() == (replaced || cleared ? 0u : 1u));
  CHECK(replacement_callbacks.load() == (replaced ? 1u : 0u));
  CHECK(decoder.TryReadFrame().status == AvDecoderReadStatus::kNull);
  CHECK_FALSE(decoder.TryPushPacket(source.packets.front()));
  CHECK_FALSE(decoder.TryPushEof());
  CHECK_FALSE(decoder.TryReset());
  std::lock_guard<std::mutex> lock(mutex);
  if (!replaced && !cleared) {
    CHECK(callback_error_matches);
    CHECK(callback_read_status == AvDecoderReadStatus::kNull);
  }
  const auto retained_error = decoder.error();
  CHECK(observed == (cleared ? nullptr : retained_error));
  REQUIRE(retained_error != nullptr);
  try {
    std::rethrow_exception(retained_error);
  } catch (const ffmpeg::FfmpegException& error) {
    CHECK(error.error_code() == native_error);
  }
}

}  // namespace

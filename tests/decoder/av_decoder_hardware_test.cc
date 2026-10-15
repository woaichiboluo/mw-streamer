#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mw/streamer/decoder/av_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include <fmt/format.h>

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/hw_device_context.h"

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

Source ReadSource(std::string_view file,
                  AVMediaType type = AVMEDIA_TYPE_VIDEO) {
  const auto path = fmt::format("{}/{}", MW_AV_DECODER_TEST_DATA_DIR, file);
  AVFormatContext* input = nullptr;
  const int opened =
      avformat_open_input(&input, path.c_str(), nullptr, nullptr);
  std::unique_ptr<AVFormatContext, FormatDeleter> owner(input);
  REQUIRE(opened >= 0);
  REQUIRE(avformat_find_stream_info(input, nullptr) >= 0);
  const int index = av_find_best_stream(input, type, -1, -1, nullptr, 0);
  REQUIRE(index >= 0);
  const auto* stream = input->streams[index];
  Source source;
  source.stream = {index, ffmpeg::CodecParameters(*stream->codecpar),
                   stream->time_base};
  source.stream.frame_rate =
      av_guess_frame_rate(input, input->streams[index], nullptr);
  source.stream.sample_aspect_ratio =
      av_guess_sample_aspect_ratio(input, input->streams[index], nullptr);
  for (;;) {
    ffmpeg::Packet packet;
    const int result = av_read_frame(input, packet.get());
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
  int width = 0;
  int height = 0;
  int samples = 0;
  int channels = 0;
  std::vector<std::uint16_t> pixels;
  std::vector<std::uint8_t> audio;

  bool operator==(const FrameRecord& other) const {
    return pts == other.pts && width == other.width && height == other.height &&
           samples == other.samples && channels == other.channels &&
           pixels == other.pixels && audio == other.audio;
  }
};

// Compare Y/U/V sample values, independent of planar, NV12 or P010 storage.
FrameRecord Record(const AVFrame& frame, std::int64_t pts) {
  FrameRecord record;
  record.pts = pts;
  record.width = frame.width;
  record.height = frame.height;
  record.samples = frame.nb_samples;
  record.channels = frame.ch_layout.nb_channels;
  if (frame.nb_samples > 0) {
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const int planes = planar ? frame.ch_layout.nb_channels : 1;
    const int bytes = frame.nb_samples * av_get_bytes_per_sample(format) *
                      (planar ? 1 : frame.ch_layout.nb_channels);
    REQUIRE(bytes > 0);
    for (int plane = 0; plane < planes; ++plane) {
      record.audio.insert(record.audio.end(), frame.extended_data[plane],
                          frame.extended_data[plane] + bytes);
    }
    return record;
  }
  const auto* description =
      av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
  REQUIRE(description != nullptr);
  REQUIRE(description->nb_components == 3);
  REQUIRE((description->flags & AV_PIX_FMT_FLAG_BE) == 0);
  for (int index = 0; index < description->nb_components; ++index) {
    const auto& component = description->comp[index];
    REQUIRE(component.depth > 0);
    REQUIRE(component.depth <= 16);
    const int horizontal_shift = index == 0 ? 0 : description->log2_chroma_w;
    const int vertical_shift = index == 0 ? 0 : description->log2_chroma_h;
    const int width =
        (frame.width + (1 << horizontal_shift) - 1) >> horizontal_shift;
    const int height =
        (frame.height + (1 << vertical_shift) - 1) >> vertical_shift;
    for (int row = 0; row < height; ++row) {
      for (int column = 0; column < width; ++column) {
        const auto* sample = frame.data[component.plane] +
                             row * frame.linesize[component.plane] +
                             column * component.step + component.offset;
        int value = sample[0];
        if (component.depth > 8) value |= sample[1] << 8;
        value = (value >> component.shift) & ((1 << component.depth) - 1);
        record.pixels.push_back(static_cast<std::uint16_t>(value));
      }
    }
  }
  return record;
}

std::vector<FrameRecord> ReferenceDecode(const Source& source) {
  const auto* codec =
      avcodec_find_decoder(source.stream.codec_parameters.get()->codec_id);
  REQUIRE(codec != nullptr);
  std::unique_ptr<AVCodecContext, CodecDeleter> context(
      avcodec_alloc_context3(codec));
  REQUIRE(context != nullptr);
  REQUIRE(avcodec_parameters_to_context(
              context.get(), source.stream.codec_parameters.get()) >= 0);
  context->pkt_timebase = source.stream.time_base;
  context->framerate = source.stream.frame_rate;
  context->thread_count = 1;
  REQUIRE(avcodec_open2(context.get(), codec, nullptr) >= 0);
  std::vector<FrameRecord> records;
  ffmpeg::Frame frame;
  const auto receive = [&] {
    for (;;) {
      const int code = avcodec_receive_frame(context.get(), frame.get());
      if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
      REQUIRE(code >= 0);
      records.push_back(Record(*frame.get(), frame->best_effort_timestamp));
    }
  };
  for (const auto& packet : source.packets) {
    REQUIRE(avcodec_send_packet(context.get(), packet.get()) >= 0);
    REQUIRE(receive() == AVERROR(EAGAIN));
  }
  REQUIRE(avcodec_send_packet(context.get(), nullptr) >= 0);
  REQUIRE(receive() == AVERROR_EOF);
  REQUIRE_FALSE(records.empty());
  return records;
}

AvDecoderReadResult ReadAvailable(AvDecoder& decoder) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    auto result = decoder.TryReadFrame();
    REQUIRE(decoder.error() == nullptr);
    if (result.status != AvDecoderReadStatus::kNull) return result;
    REQUIRE(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
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

FrameRecord Download(const ffmpeg::Frame& frame) {
  ffmpeg::Frame downloaded;
  REQUIRE(av_hwframe_transfer_data(downloaded.get(), frame.get(), 0) >= 0);
  REQUIRE((downloaded->format == AV_PIX_FMT_NV12 ||
           downloaded->format == AV_PIX_FMT_P010LE));
  return Record(*downloaded.get(), frame->pts);
}

TEST_CASE(
    "AvDecoder CPU device and explicit software names preserve decoded content",
    "[av-decoder][hardware][cpu]") {
  ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
  for (const auto file : {"h264_aac.mp4", "h265_cuda.mp4"}) {
    CAPTURE(file);
    const auto source = ReadSource(file);
    const auto expected = ReferenceDecode(source);
    AvDecoderConfig config;
    config.thread_count = 1;
    config.decoder_name =
        source.stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264
            ? "h264"
            : "hevc";
    AvDecoder decoder(source.stream, cpu, config);
    SourceFeeder feeder(decoder, source);
    for (const auto& frame : expected) {
      const auto result = ReadAvailable(decoder);
      REQUIRE(result.status == AvDecoderReadStatus::kFrame);
      REQUIRE(result.frame.has_value());
      CHECK(result.frame->get()->format != AV_PIX_FMT_CUDA);
      CHECK(result.frame->get()->hw_frames_ctx == nullptr);
      CHECK(Record(*result.frame->get(), result.frame->get()->pts) == frame);
    }
    CHECK(ReadAvailable(decoder).status == AvDecoderReadStatus::kEof);
    feeder.Join();
  }
}

TEST_CASE(
    "AvDecoder rejects unavailable, mismatched and embedded-NUL decoder names",
    "[av-decoder][hardware][validation]") {
  const auto source = ReadSource("h264_aac.mp4");
  AvDecoderConfig config;
  SECTION("unknown decoder") {
    config.decoder_name = "mw_missing_decoder";
    try {
      AvDecoder decoder(source.stream, config);
      FAIL("Unknown decoder must fail synchronously");
    } catch (const ffmpeg::FfmpegException& error) {
      CHECK(error.error_code() == AVERROR_DECODER_NOT_FOUND);
    }
  }
  SECTION("different video codec") {
    config.decoder_name = "hevc";
    CHECK_THROWS_AS(AvDecoder(source.stream, config), std::invalid_argument);
  }
  SECTION("audio decoder for video") {
    config.decoder_name = "aac";
    CHECK_THROWS_AS(AvDecoder(source.stream, config), std::invalid_argument);
  }
  SECTION("embedded NUL") {
    config.decoder_name = std::string("h264\0suffix", 11);
    CHECK_THROWS_AS(AvDecoder(source.stream, config), std::invalid_argument);
  }
  SECTION("hardware decoder requires an external device") {
    config.decoder_name = "h264_cuvid";
    ffmpeg::HwDeviceContext cpu(ffmpeg::HwDeviceType::kCpu);
    CHECK_THROWS(AvDecoder(source.stream, config));
    CHECK_THROWS(AvDecoder(source.stream, cpu, config));
  }
}

TEST_CASE(
    "AvDecoder CUDA native and cuvid workers share one device across "
    "ordered resets",
    "[.][av-decoder][cuda]") {
  bool explicit_native = true;
  SECTION("explicit native decoder") {}
  SECTION("automatic native decoder") { explicit_native = false; }
  std::vector<ffmpeg::Frame> retained;
  std::vector<FrameRecord> retained_records;
  {
    ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
    const auto* shared_device = device.get()->data;
    for (const auto file :
         {"h264_aac.mp4", "h264_cuda_dpb.mp4", "h265_cuda.mp4"}) {
      CAPTURE(file);
      const auto source = ReadSource(file);
      const auto expected = ReferenceDecode(source);
      const std::string native_name =
          source.stream.codec_parameters.get()->codec_id == AV_CODEC_ID_H264
              ? "h264"
              : "hevc";
      AvDecoderConfig native_config;
      if (explicit_native) native_config.decoder_name = native_name;
      AvDecoderConfig cuvid_config = native_config;
      cuvid_config.decoder_name = native_name + "_cuvid";
      cuvid_config.thread_count = 2;
      AvDecoder native(source.stream, device, native_config);
      AvDecoder cuvid(source.stream, device, cuvid_config);
      for (int replay = 0; replay < 2; ++replay) {
        if (replay != 0) {
          REQUIRE(native.TryReset());
          REQUIRE(cuvid.TryReset());
        }
        SourceFeeder native_feeder(native, source);
        SourceFeeder cuvid_feeder(cuvid, source);
        for (std::size_t index = 0; index < expected.size(); ++index) {
          for (auto* decoder : {&native, &cuvid}) {
            const auto result = ReadAvailable(*decoder);
            REQUIRE(result.status == AvDecoderReadStatus::kFrame);
            REQUIRE(result.frame.has_value());
            CHECK(result.stream_index == source.stream.stream_index);
            CHECK(result.time_base.num == source.stream.time_base.num);
            CHECK(result.time_base.den == source.stream.time_base.den);
            REQUIRE(result.frame->get()->format == AV_PIX_FMT_CUDA);
            REQUIRE(result.frame->get()->hw_frames_ctx != nullptr);
            const auto* pool = reinterpret_cast<const AVHWFramesContext*>(
                result.frame->get()->hw_frames_ctx->data);
            REQUIRE(pool->device_ref != nullptr);
            CHECK(pool->device_ref->data == shared_device);
            CHECK(Download(*result.frame) == expected[index]);
            if (replay == 0 && index == 0) {
              retained.push_back(result.frame->Ref());
              retained_records.push_back(expected[index]);
            }
          }
        }
        for (auto* decoder : {&native, &cuvid}) {
          const auto eof = ReadAvailable(*decoder);
          CHECK(eof.status == AvDecoderReadStatus::kEof);
          CHECK_FALSE(eof.frame.has_value());
          CHECK(decoder->TryReadFrame().status == AvDecoderReadStatus::kNull);
        }
        native_feeder.Join();
        cuvid_feeder.Join();
      }
      native.Stop();
      cuvid.Stop();
    }
  }
  REQUIRE(retained.size() == retained_records.size());
  REQUIRE(retained.size() == 6);
  for (std::size_t index = 0; index < retained.size(); ++index) {
    CHECK(Download(retained[index]) == retained_records[index]);
  }
}

TEST_CASE(
    "AvDecoder CUDA device keeps audio on CPU and rejects unsupported codecs",
    "[.][av-decoder][cuda]") {
  ffmpeg::HwDeviceContext device(ffmpeg::HwDeviceType::kCuda);
  const auto audio = ReadSource("h264_aac.mp4", AVMEDIA_TYPE_AUDIO);
  const auto expected = ReferenceDecode(audio);
  AvDecoder decoder(audio.stream, device);
  SourceFeeder feeder(decoder, audio);
  for (const auto& frame : expected) {
    const auto result = ReadAvailable(decoder);
    REQUIRE(result.status == AvDecoderReadStatus::kFrame);
    REQUIRE(result.frame.has_value());
    CHECK(result.frame->get()->hw_frames_ctx == nullptr);
    CHECK(Record(*result.frame->get(), result.frame->get()->pts) == frame);
  }
  CHECK(ReadAvailable(decoder).status == AvDecoderReadStatus::kEof);
  feeder.Join();
  auto unsupported = ReadSource("h264_aac.mp4").stream;
  unsupported.codec_parameters.get()->codec_id = AV_CODEC_ID_RAWVIDEO;
  unsupported.codec_parameters.get()->format = AV_PIX_FMT_YUV420P;
  CHECK_THROWS(AvDecoder(unsupported, device));
  CHECK_THROWS_AS(ffmpeg::HwDeviceContext(ffmpeg::HwDeviceType::kCuda, "99999"),
                  ffmpeg::FfmpegException);
}

}  // namespace

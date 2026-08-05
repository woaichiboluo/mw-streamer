#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <type_traits>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include "mw/ffmpeg/codec_context.h"
#include "mw/ffmpeg/codec_parameters.h"
#include "mw/ffmpeg/error.h"
#include "mw/ffmpeg/frame.h"
#include "mw/ffmpeg/input_format_context.h"
#include "mw/ffmpeg/packet.h"
#include "mw/ffmpeg/pixel_format.h"
#include "mw/ffmpeg/stream_info.h"

namespace {

using mw::streamer::ffmpeg::CodecContext;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::ErrorText;
using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::InputFormatContext;
using mw::streamer::ffmpeg::IsHardwarePixelFormat;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::ffmpeg::ThrowIfError;

static_assert(std::is_copy_constructible_v<CodecParameters>);
static_assert(std::is_copy_assignable_v<CodecParameters>);
static_assert(std::is_move_constructible_v<CodecParameters>);

static_assert(std::is_copy_constructible_v<Packet>);
static_assert(std::is_copy_assignable_v<Packet>);
static_assert(std::is_nothrow_move_constructible_v<Packet>);
static_assert(std::is_nothrow_move_assignable_v<Packet>);

static_assert(std::is_copy_constructible_v<Frame>);
static_assert(std::is_copy_assignable_v<Frame>);
static_assert(std::is_nothrow_move_constructible_v<Frame>);
static_assert(std::is_nothrow_move_assignable_v<Frame>);

static_assert(!std::is_copy_constructible_v<CodecContext>);
static_assert(!std::is_copy_assignable_v<CodecContext>);
static_assert(std::is_nothrow_move_constructible_v<CodecContext>);
static_assert(std::is_nothrow_move_assignable_v<CodecContext>);

static_assert(!std::is_copy_constructible_v<InputFormatContext>);
static_assert(!std::is_copy_assignable_v<InputFormatContext>);
static_assert(std::is_nothrow_move_constructible_v<InputFormatContext>);
static_assert(std::is_nothrow_move_assignable_v<InputFormatContext>);

std::filesystem::path SamplePath(const char* name = "h264_aac.mp4") {
  return std::filesystem::path(MW_FFMPEG_TYPES_TEST_DATA_DIR) / name;
}

int NeverInterrupt(void* opaque) { return opaque ? 0 : 1; }

}  // namespace

TEST_CASE("CodecParameters has deep-copy value semantics") {
  CodecParameters source;
  source.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  source.get()->codec_id = AV_CODEC_ID_AAC;
  source.get()->extradata_size = 2;
  source.get()->extradata =
      static_cast<std::uint8_t*>(av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE));
  REQUIRE(source.get()->extradata);
  source.get()->extradata[0] = 0x12;
  source.get()->extradata[1] = 0x10;

  CodecParameters copy(source);
  REQUIRE(copy.get());
  CHECK(copy.get() != source.get());
  CHECK(copy.get()->extradata != source.get()->extradata);
  REQUIRE(copy.get()->extradata_size == source.get()->extradata_size);
  CHECK(std::memcmp(copy.get()->extradata, source.get()->extradata, 2) == 0);

  copy.get()->extradata[0] = 0xff;
  CHECK(source.get()->extradata[0] == 0x12);

  CodecParameters assigned;
  assigned = source;
  CHECK(assigned.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(assigned.get()->extradata != source.get()->extradata);
}

TEST_CASE("InputFormatContext owns an input and reads packets to EOF") {
  int callback_context = 1;
  InputFormatContext input(SamplePath().string(),
                           {NeverInterrupt, &callback_context});
  REQUIRE(input.get());
  CHECK(input->interrupt_callback.callback == NeverInterrupt);
  CHECK(input->interrupt_callback.opaque == &callback_context);

  input.FindStreamInfo();
  CHECK(input->nb_streams == 2);

  Packet packet;
  std::size_t packet_count = 0;
  while (input.ReadPacket(packet)) {
    ++packet_count;
    packet.Unref();
  }
  CHECK(packet_count > 0);
}

TEST_CASE("InputFormatContext supports move-only ownership") {
  InputFormatContext source(SamplePath().string());
  InputFormatContext moved(std::move(source));
  CHECK(source.get() == nullptr);
  REQUIRE(moved.get());

  InputFormatContext assigned(SamplePath("h264_video.mp4").string());
  assigned = std::move(moved);
  CHECK(moved.get() == nullptr);
  REQUIRE(assigned.get());
  assigned.FindStreamInfo();

  Packet packet;
  CHECK(assigned.ReadPacket(packet));
  CHECK_THROWS_AS(source.FindStreamInfo(), std::logic_error);
}

TEST_CASE("InputFormatContext rejects invalid inputs") {
  CHECK_THROWS_AS(InputFormatContext(""), std::invalid_argument);
  CHECK_THROWS_AS(InputFormatContext(SamplePath("missing-input.mp4").string()),
                  std::runtime_error);
}

TEST_CASE("Packet Clone owns metadata and references the media buffer") {
  Packet source;
  REQUIRE(av_new_packet(source.get(), 4) >= 0);
  source->data[0] = 1;
  source->pts = 10;

  auto clone = source.Clone();
  REQUIRE(clone.get());
  CHECK(clone.get() != source.get());
  CHECK(clone->data == source->data);
  CHECK(clone->pts == source->pts);

  clone->pts = 20;
  clone->data[0] = 2;
  CHECK(source->pts == 10);
  CHECK(source->data[0] == 2);

  Packet moved(std::move(clone));
  CHECK(clone.get() == nullptr);
  CHECK(moved->pts == 20);
}

TEST_CASE("Packet supports adopt and FFmpeg reference-copy semantics") {
  auto* raw_packet = av_packet_alloc();
  REQUIRE(raw_packet);
  Packet adopted(raw_packet);
  CHECK(adopted.get() == raw_packet);
  CHECK_THROWS_AS(Packet(static_cast<AVPacket*>(nullptr)),
                  std::invalid_argument);

  REQUIRE(av_new_packet(adopted.get(), 4) >= 0);
  adopted->data[0] = 1;
  adopted->pts = 10;

  Packet copy(adopted);
  CHECK(copy.get() != adopted.get());
  CHECK(copy->data == adopted->data);
  CHECK(copy->pts == adopted->pts);

  Packet assigned;
  assigned = adopted;
  CHECK(assigned.get() != adopted.get());
  CHECK(assigned->data == adopted->data);

  auto referenced = adopted.Ref();
  CHECK(referenced.get() != adopted.get());
  CHECK(referenced->data == adopted->data);

  adopted.Unref();
  REQUIRE(copy->data);
  CHECK(copy->data[0] == 1);
  CHECK(assigned->data == copy->data);
  CHECK(referenced->data == copy->data);
}

TEST_CASE("Frame Clone owns metadata and references frame buffers") {
  Frame source;
  source->format = AV_SAMPLE_FMT_FLTP;
  source->sample_rate = 48000;
  source->nb_samples = 32;
  av_channel_layout_default(&source->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(source.get(), 0) >= 0);
  source->pts = 100;

  auto clone = source.Clone();
  REQUIRE(clone.get());
  CHECK(clone.get() != source.get());
  CHECK(clone->extended_data[0] == source->extended_data[0]);
  CHECK(clone->pts == source->pts);

  clone->pts = 200;
  CHECK(source->pts == 100);

  Frame moved(std::move(clone));
  CHECK(clone.get() == nullptr);
  CHECK(moved->pts == 200);
}

TEST_CASE("Frame supports adopt and FFmpeg reference-copy semantics") {
  auto* raw_frame = av_frame_alloc();
  REQUIRE(raw_frame);
  Frame adopted(raw_frame);
  CHECK(adopted.get() == raw_frame);
  CHECK_THROWS_AS(Frame(static_cast<AVFrame*>(nullptr)), std::invalid_argument);

  adopted->format = AV_SAMPLE_FMT_FLTP;
  adopted->sample_rate = 48000;
  adopted->nb_samples = 32;
  av_channel_layout_default(&adopted->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(adopted.get(), 0) >= 0);
  adopted->pts = 100;

  Frame copy(adopted);
  CHECK(copy.get() != adopted.get());
  CHECK(copy->extended_data[0] == adopted->extended_data[0]);
  CHECK(copy->pts == adopted->pts);

  Frame assigned;
  assigned = adopted;
  CHECK(assigned.get() != adopted.get());
  CHECK(assigned->extended_data[0] == adopted->extended_data[0]);

  auto referenced = adopted.Ref();
  CHECK(referenced.get() != adopted.get());
  CHECK(referenced->extended_data[0] == adopted->extended_data[0]);

  adopted.Unref();
  REQUIRE(copy->extended_data[0]);
  CHECK(assigned->extended_data[0] == copy->extended_data[0]);
  CHECK(referenced->extended_data[0] == copy->extended_data[0]);
}

TEST_CASE("Frame copies properties without replacing media storage") {
  Frame source;
  source->pts = 100;
  source->duration = 4;
  source->time_base = {1, 25};
  source->color_range = AVCOL_RANGE_MPEG;
  source->crop_top = 2;

  Frame destination;
  destination->format = AV_SAMPLE_FMT_FLTP;
  destination->sample_rate = 48000;
  destination->nb_samples = 32;
  av_channel_layout_default(&destination->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(destination.get(), 0) >= 0);
  auto* const data = destination->extended_data[0];

  destination.CopyPropertiesFrom(source);

  CHECK(destination->pts == 100);
  CHECK(destination->duration == 4);
  CHECK(destination->time_base.num == 1);
  CHECK(destination->time_base.den == 25);
  CHECK(destination->color_range == AVCOL_RANGE_MPEG);
  CHECK(destination->crop_top == 2);
  CHECK(destination->extended_data[0] == data);

  destination.ClearCrop();
  CHECK(destination->crop_top == 0);
  CHECK(destination->crop_bottom == 0);
  CHECK(destination->crop_left == 0);
  CHECK(destination->crop_right == 0);
}

TEST_CASE("FFmpeg common validation and error helpers are reusable") {
  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters.get()->codec_id = AV_CODEC_ID_AAC;
  StreamInfo stream_info{3, parameters, {1, 48000}};

  CHECK_NOTHROW(stream_info.Validate());
  stream_info.stream_index = -1;
  CHECK_THROWS_AS(stream_info.Validate(), std::invalid_argument);

  CHECK_NOTHROW(ThrowIfError(0, "测试操作"));
  CHECK_FALSE(ErrorText(AVERROR(EINVAL)).empty());
  CHECK_THROWS_AS(ThrowIfError(AVERROR(EINVAL), "测试操作"),
                  std::runtime_error);
}

TEST_CASE("StreamInfo can be exported from an AVCodecContext") {
  const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  REQUIRE(codec);
  CodecContext context(codec);
  context.get()->time_base = {1, 48000};

  auto stream_info = StreamInfo::FromCodecContext(*context.get(), 4);

  CHECK(stream_info.stream_index == 4);
  CHECK(stream_info.codec_parameters.get()->codec_type == AVMEDIA_TYPE_AUDIO);
  CHECK(stream_info.codec_parameters.get()->codec_id == AV_CODEC_ID_AAC);
  CHECK(stream_info.time_base.num == 1);
  CHECK(stream_info.time_base.den == 48000);
}

TEST_CASE("FFmpeg hardware pixel format detection is centralized") {
  CHECK(IsHardwarePixelFormat(AV_PIX_FMT_CUDA));
  CHECK_FALSE(IsHardwarePixelFormat(AV_PIX_FMT_YUV420P));
  CHECK_FALSE(IsHardwarePixelFormat(AV_PIX_FMT_NONE));
}

TEST_CASE("CodecContext has unique movable ownership") {
  const auto* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  REQUIRE(codec);

  CodecContext context(codec);
  REQUIRE(context.get());
  CodecContext moved(std::move(context));
  CHECK(context.get() == nullptr);
  CHECK(moved.get() != nullptr);
}

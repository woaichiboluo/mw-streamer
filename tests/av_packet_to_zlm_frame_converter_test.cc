#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "mw/converter/av_packet_to_zlm_frame_converter.h"
#include "mw/ffmpeg/codec_parameters.h"
#include "mw/ffmpeg/packet.h"

namespace {

using mw::streamer::converter::AvPacketToZlmFrameConverter;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;

Packet MakePacket(const std::vector<std::uint8_t>& payload, int stream_index,
                  std::int64_t dts, std::int64_t pts, int flags = 0) {
  Packet packet;
  REQUIRE(av_new_packet(packet.get(), static_cast<int>(payload.size())) >= 0);
  std::memcpy(packet.get()->data, payload.data(), payload.size());
  packet.get()->stream_index = stream_index;
  packet.get()->dts = dts;
  packet.get()->pts = pts;
  packet.get()->flags = flags;
  return packet;
}

CodecParameters MakeCodecParameters(AVCodecID codec_id) {
  CodecParameters parameters;
  parameters.get()->codec_id = codec_id;
  return parameters;
}

}  // namespace

TEST_CASE("AVPacket转换为可缓存的H264 ZLM Frame") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_H264);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 90000}, 3);
  const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01,
                                          0x65, 0x88, 0x84};

  auto packet = MakePacket(payload, 3, 90000, 94500, AV_PKT_FLAG_KEY);
  auto frames = converter.Convert(packet);
  REQUIRE(frames.size() == 1);
  auto frame = frames.front();

  packet = Packet();
  CHECK(frame->getCodecId() == mediakit::CodecH264);
  CHECK(frame->getIndex() == 3);
  CHECK(frame->dts() == 1000);
  CHECK(frame->pts() == 1050);
  CHECK(frame->prefixSize() == 4);
  CHECK(frame->keyFrame());
  CHECK(frame->cacheAble());
  REQUIRE(frame->size() == payload.size());
  CHECK(std::memcmp(frame->data(), payload.data(), payload.size()) == 0);
}

TEST_CASE("AVPacket转换器移动接管Packet且不增加Buffer引用") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_H264);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 0);
  const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01, 0x65};
  auto packet = MakePacket(payload, 0, 0, 0, AV_PKT_FLAG_KEY);
  auto* buffer = packet.get()->buf;
  REQUIRE(buffer);
  REQUIRE(av_buffer_get_ref_count(buffer) == 1);

  auto frames = converter.Convert(std::move(packet));

  CHECK(packet.get() == nullptr);
  REQUIRE(frames.size() == 1);
  CHECK(av_buffer_get_ref_count(buffer) == 1);
  CHECK(std::memcmp(frames.front()->data(), payload.data(), payload.size()) ==
        0);
}

TEST_CASE("AVPacket转换为H265 ZLM Frame") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_HEVC);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 0);
  const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01,
                                          0x26, 0x01, 0x88};
  auto packet = MakePacket(payload, 0, 25, 40, AV_PKT_FLAG_KEY);

  auto frames = converter.Convert(packet);

  REQUIRE(frames.size() == 1);
  const auto& frame = frames.front();
  CHECK(frame->getCodecId() == mediakit::CodecH265);
  CHECK(frame->getIndex() == 0);
  CHECK(frame->dts() == 25);
  CHECK(frame->pts() == 40);
  CHECK(frame->prefixSize() == 4);
  CHECK(frame->keyFrame());
}

TEST_CASE("裸AAC AVPacket使用ASC生成ADTS ZLM Frame") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_AAC);
  const std::uint8_t aac_config[]{0x12, 0x10};
  AVCodecParameters source{};
  source.codec_id = AV_CODEC_ID_AAC;
  source.extradata = const_cast<std::uint8_t*>(aac_config);
  source.extradata_size = sizeof(aac_config);
  parameters = CodecParameters(source);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 48000}, 1);
  const std::vector<std::uint8_t> payload{0x21, 0x10, 0x56, 0xe5};
  auto packet = MakePacket(payload, 1, 48000, 48000);

  auto frames = converter.Convert(packet);

  REQUIRE(frames.size() == 1);
  const auto& frame = frames.front();
  CHECK(frame->getCodecId() == mediakit::CodecAAC);
  CHECK(frame->getIndex() == 1);
  CHECK(frame->dts() == 1000);
  CHECK(frame->pts() == 1000);
  CHECK(frame->prefixSize() == 7);
  REQUIRE(frame->size() == payload.size() + 7);
  CHECK(static_cast<std::uint8_t>(frame->data()[0]) == 0xff);
  CHECK((static_cast<std::uint8_t>(frame->data()[1]) & 0xf0) == 0xf0);
  CHECK(std::memcmp(frame->data() + 7, payload.data(), payload.size()) == 0);
}

TEST_CASE("已有ADTS的AAC AVPacket不会重复添加头部") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_AAC);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 1);
  const std::vector<std::uint8_t> payload{0xff, 0xf1, 0x50, 0x80, 0x01, 0x7f,
                                          0xfc, 0x21, 0x10, 0x56, 0xe5};
  auto packet = MakePacket(payload, 1, 1000, 1000);

  auto frames = converter.Convert(packet);

  REQUIRE(frames.size() == 1);
  const auto& frame = frames.front();
  CHECK(frame->prefixSize() == 7);
  REQUIRE(frame->size() == payload.size());
  CHECK(std::memcmp(frame->data(), payload.data(), payload.size()) == 0);
}

TEST_CASE("AVPacket转换器拒绝错误的流和时间戳") {
  auto parameters = MakeCodecParameters(AV_CODEC_ID_H264);
  AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 0);
  const std::vector<std::uint8_t> payload{0x00, 0x00, 0x00, 0x01, 0x65};

  auto wrong_stream = MakePacket(payload, 1, 0, 0);
  CHECK(converter.Convert(wrong_stream).empty());

  auto missing_timestamp =
      MakePacket(payload, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
  CHECK(converter.Convert(missing_timestamp).empty());

  auto negative_timestamp = MakePacket(payload, 0, -1, -1);
  CHECK(converter.Convert(negative_timestamp).empty());

  auto avcc_packet = MakePacket({0x00, 0x00, 0x00, 0x01, 0x65}, 0, 0, 0);
  avcc_packet.get()->data[3] = 0x02;
  CHECK(converter.Convert(avcc_packet).empty());
}

TEST_CASE("H264和H265 AVPacket按Annex-B NALU拆分为多个ZLM Frame") {
  SECTION("H264") {
    auto parameters = MakeCodecParameters(AV_CODEC_ID_H264);
    AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 0);
    const std::vector<std::uint8_t> payload{
        0x00, 0x00, 0x00, 0x01, 0x09, 0xf0, 0x00,
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84,
    };
    auto packet = MakePacket(payload, 0, 25, 40, AV_PKT_FLAG_KEY);

    auto frames = converter.Convert(packet);

    REQUIRE(frames.size() == 2);
    CHECK_FALSE(frames[0]->keyFrame());
    CHECK(frames[1]->keyFrame());
    CHECK(frames[0]->prefixSize() == 4);
    CHECK(frames[1]->prefixSize() == 4);
    CHECK(frames[0]->size() == 6);
    CHECK(frames[1]->size() == 7);
  }

  SECTION("H265") {
    auto parameters = MakeCodecParameters(AV_CODEC_ID_HEVC);
    AvPacketToZlmFrameConverter converter(parameters, AVRational{1, 1000}, 0);
    const std::vector<std::uint8_t> payload{
        0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50, 0x00, 0x00, 0x00, 0x01,
        0x26, 0x01, 0x80, 0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0x00,
    };
    auto packet = MakePacket(payload, 0, 25, 40, AV_PKT_FLAG_KEY);

    auto frames = converter.Convert(packet);

    REQUIRE(frames.size() == 3);
    CHECK_FALSE(frames[0]->keyFrame());
    CHECK(frames[1]->keyFrame());
    CHECK_FALSE(frames[2]->keyFrame());
    for (const auto& frame : frames) {
      CHECK(frame->prefixSize() == 4);
      CHECK(frame->size() == 7);
    }
  }
}

TEST_CASE("AVPacket转换器校验固定流参数") {
  auto unsupported = MakeCodecParameters(AV_CODEC_ID_NONE);
  CHECK_THROWS_AS(
      AvPacketToZlmFrameConverter(unsupported, AVRational{1, 1000}, 0),
      std::invalid_argument);

  auto h264 = MakeCodecParameters(AV_CODEC_ID_H264);
  CHECK_THROWS_AS(AvPacketToZlmFrameConverter(h264, AVRational{0, 1}, 0),
                  std::invalid_argument);
  CHECK_THROWS_AS(AvPacketToZlmFrameConverter(h264, AVRational{1, 1000}, -1),
                  std::invalid_argument);
}

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
}

#include "../encoder/encoder_test_support.h"
#include "ext-codec/AAC.h"
#include "ext-codec/H264.h"
#include "ext-codec/H265.h"
#include "mw/streamer/init/init.h"
#include "mw/streamer/remuxer/packet_converter.h"

namespace {

namespace ffmpeg = mw::streamer::ffmpeg;
using mw::streamer::internal::PacketConverter;

class Runtime final {
 public:
  Runtime() {
    mw::streamer::InitConfig config;
    config.event_poller_threads = 1;
    config.work_threads = 1;
    config.enable_cpu_affinity = false;
    context_ = mw::streamer::Init(config);
  }
  ~Runtime() { mw::streamer::Shutdown(context_); }

 private:
  mw::streamer::MwStreamerContext* context_;
};

class Options final {
 public:
  ~Options() { av_dict_free(&value_); }
  void Set(const char* name, const char* value) {
    ffmpeg::FfmpegException::throwIfError(av_dict_set(&value_, name, value, 0),
                                          "设置包转换测试编码选项");
  }
  AVDictionary* get() const { return value_; }

 private:
  AVDictionary* value_ = nullptr;
};

struct Encoded {
  ffmpeg::StreamInfo stream;
  std::vector<ffmpeg::Packet> packets;
};

void Encode(ffmpeg::Encoder& encoder, const ffmpeg::Frame& frame,
            Encoded& output, int index) {
  const auto receive = [&] {
    ffmpeg::Packet packet;
    while (encoder.ReceivePacket(packet) == ffmpeg::EncodeResult::kPacket) {
      packet->stream_index = index;
      output.packets.push_back(packet.Ref());
    }
  };
  while (!encoder.SendFrame(frame)) receive();
  receive();
  while (!encoder.Drain()) receive();
  receive();
  output.stream = encoder.stream_info(index);
  REQUIRE_FALSE(output.packets.empty());
}

Encoded Video(const char* name) {
  ffmpeg::VideoEncoderConfig config;
  config.encoder_name = name;
  config.width = 64;
  config.height = 64;
  config.frame_rate = {50, 1};
  config.time_base = {1, 50};
  Options options;
  options.Set("preset", "ultrafast");
  if (config.encoder_name == "libx264") {
    options.Set("tune", "zerolatency");
    options.Set("threads", "1");
  } else {
    options.Set("x265-params", "pools=none:frame-threads=1:log-level=error");
  }
  ffmpeg::VideoEncoder encoder(config, options.get());
  Encoded output;
  Encode(encoder, encoder_test::VideoFrame(0), output, 4);
  return output;
}

Encoded Audio() {
  ffmpeg::AudioEncoder encoder(ffmpeg::AudioEncoderConfig{});
  Encoded output;
  Encode(encoder, encoder_test::AudioFrame(1024), output, 7);
  return output;
}

void ExtraData(ffmpeg::StreamInfo& stream, const std::string& data) {
  auto* parameters = stream.codec_parameters.get();
  av_freep(&parameters->extradata);
  parameters->extradata_size = static_cast<int>(data.size());
  parameters->extradata = static_cast<std::uint8_t*>(
      av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  REQUIRE(parameters->extradata != nullptr);
  std::memcpy(parameters->extradata, data.data(), data.size());
}

ffmpeg::Packet Packet(const ffmpeg::StreamInfo& stream,
                      const std::string& data) {
  ffmpeg::Packet packet;
  ffmpeg::FfmpegException::throwIfError(
      av_new_packet(packet.get(), static_cast<int>(data.size())),
      "分配包转换测试数据");
  std::memcpy(packet->data, data.data(), data.size());
  packet->stream_index = stream.stream_index;
  packet->time_base = stream.time_base;
  packet->pts = 0;
  packet->dts = 0;
  return packet;
}

std::vector<std::string> Nals(const ffmpeg::Packet& packet) {
  std::vector<std::string> result;
  const auto* data = reinterpret_cast<const char*>(packet->data);
  mediakit::splitH264(
      data, static_cast<std::size_t>(packet->size),
      mediakit::prefixSize(data, static_cast<std::size_t>(packet->size)),
      [&](const char* nal, std::size_t size, std::size_t prefix) {
        result.emplace_back(nal + prefix, size - prefix);
      });
  return result;
}

TEST_CASE("真实H264与H265配置准备参数集并保留包缓存和毫秒时间",
          "[remuxer][shared][packet][video]") {
  Runtime runtime;
  const char* name = "libx264";
  SECTION("H264") {}
  SECTION("H265") { name = "libx265"; }
  auto encoded = Video(name);
  PacketConverter converter({encoded.stream});
  REQUIRE(converter.tracks().size() == 1);
  const auto& track = converter.tracks().front();
  REQUIRE(track->ready());
  CHECK(track->getIndex() == 4);
  const auto video = std::dynamic_pointer_cast<mediakit::VideoTrack>(track);
  REQUIRE(video != nullptr);
  CHECK(video->getVideoWidth() == 64);
  CHECK(video->getVideoHeight() == 64);
  const auto configs = video->getConfigFrames();
  REQUIRE(configs.size() ==
          (track->getCodecId() == mediakit::CodecH264 ? 2 : 3));
  for (const auto& config : configs) {
    CHECK(config->configFrame());
    CHECK(config->getIndex() == 4);
  }

  auto& packet = encoded.packets.front();
  const auto* data = packet->data;
  const std::string original(reinterpret_cast<const char*>(data),
                             static_cast<std::size_t>(packet->size));
  auto frame = converter.Convert(packet, 123456, 123496);
  CHECK(frame->cacheAble());
  CHECK(frame->data() == reinterpret_cast<const char*>(data));
  CHECK(frame->dts() == 123456);
  CHECK(frame->pts() == 123496);
  CHECK(frame->getIndex() == 4);
  CHECK(frame->getCodecId() == track->getCodecId());
  std::vector<mediakit::Frame::Ptr> emitted;
  auto* delegate = track->addDelegate([&](const auto& value) {
    emitted.push_back(value);
    return true;
  });
  REQUIRE(track->inputFrame(frame));
  CHECK(std::any_of(emitted.begin(), emitted.end(),
                    [](const auto& value) { return value->keyFrame(); }));
  packet.Unref();
  CHECK(frame->toString() == original);
  for (const auto& value : emitted) {
    CHECK(value->getIndex() == 4);
    CHECK(value->dts() == 123456);
    CHECK(value->pts() == 123496);
  }
  track->delDelegate(delegate);
}

TEST_CASE("avcC与hvcC兼容一至四字节NAL长度和Annex B输入",
          "[remuxer][shared][packet][length-prefix]") {
  Runtime runtime;
  const char* name = "libx264";
  SECTION("avcC") {}
  SECTION("hvcC") { name = "libx265"; }
  auto encoded = Video(name);
  PacketConverter original({encoded.stream});
  const auto codec = original.tracks().front()->getCodecId();
  const auto configuration =
      original.tracks().front()->getExtraData()->toString();
  const auto nals = Nals(encoded.packets.front());
  const auto key = std::find_if(nals.begin(), nals.end(), [&](const auto& nal) {
    const auto type = static_cast<unsigned char>(nal[0]);
    return codec == mediakit::CodecH264
               ? (type & 0x1f) == 5
               : ((type >> 1) & 0x3f) >= 16 && ((type >> 1) & 0x3f) <= 23;
  });
  REQUIRE(key != nals.end());
  REQUIRE(key->size() < 256);
  for (int width : {1, 2, 3, 4}) {
    auto stream = encoded.stream;
    auto config = configuration;
    const auto position = codec == mediakit::CodecH264 ? 4U : 21U;
    config[position] = static_cast<char>(
        (static_cast<unsigned char>(config[position]) & 0xfc) | (width - 1));
    ExtraData(stream, config);
    PacketConverter converter({stream});
    REQUIRE(converter.tracks().front()->ready());
    std::string lengths;
    std::string annex_b;
    for (int count = 0; count < 2; ++count) {
      for (int byte = width - 1; byte >= 0; --byte)
        lengths.push_back(static_cast<char>(key->size() >> (byte * 8)));
      lengths += *key;
      annex_b.append("\0\0\0\1", 4);
      annex_b += *key;
    }
    auto packet = Packet(stream, lengths);
    auto frame = converter.Convert(packet, 20, 40);
    CHECK(frame->toString() == annex_b);
    CHECK(frame->keyFrame());
    CHECK(frame->getIndex() == 4);
    CHECK(frame->prefixSize() == 4);
    packet.Unref();
    CHECK(frame->toString() == annex_b);
    CHECK(converter.Convert(encoded.packets.front(), 20, 40)->toString() ==
          std::string(
              reinterpret_cast<const char*>(encoded.packets.front()->data),
              static_cast<std::size_t>(encoded.packets.front()->size)));
  }
  auto stream = encoded.stream;
  auto config = configuration;
  const auto position = codec == mediakit::CodecH264 ? 4U : 21U;
  config[position] = static_cast<char>(
      (static_cast<unsigned char>(config[position]) & 0xfc) | 3);
  ExtraData(stream, config);
  PacketConverter converter({stream});
  auto padded = *key;
  padded.resize(256, '\x55');
  const auto ambiguous = Packet(stream, std::string("\0\0\1\0", 4) + padded);
  CHECK(converter.Convert(ambiguous, 20, 40)->toString() ==
        std::string("\0\0\0\1", 4) + padded);
  auto truncated = Packet(stream, std::string("\0\0\0\x7f\x65\x80", 6));
  CHECK_THROWS_AS(converter.Convert(truncated, 20, 40), std::invalid_argument);
}

TEST_CASE("AAC使用真实ASC并由Track生成一次ADTS头",
          "[remuxer][shared][packet][audio]") {
  Runtime runtime;
  auto encoded = Audio();
  PacketConverter converter({encoded.stream});
  const auto& track = converter.tracks().front();
  REQUIRE(track->ready());
  REQUIRE(track->getCodecId() == mediakit::CodecAAC);
  const auto audio = std::dynamic_pointer_cast<mediakit::AudioTrack>(track);
  REQUIRE(audio != nullptr);
  CHECK(audio->getAudioSampleRate() == 48000);
  CHECK(audio->getAudioChannel() == 2);
  CHECK(
      track->getExtraData()->toString() ==
      std::string(reinterpret_cast<const char*>(
                      encoded.stream.codec_parameters.get()->extradata),
                  static_cast<std::size_t>(
                      encoded.stream.codec_parameters.get()->extradata_size)));
  auto& packet = encoded.packets.front();
  const std::string payload(reinterpret_cast<const char*>(packet->data),
                            static_cast<std::size_t>(packet->size));
  auto raw = converter.Convert(packet, 321, 321);
  CHECK(raw->prefixSize() == 0);
  CHECK(raw->data() == reinterpret_cast<const char*>(packet->data));
  mediakit::Frame::Ptr adts;
  auto* delegate = track->addDelegate([&](const auto& frame) {
    adts = frame;
    return true;
  });
  REQUIRE(track->inputFrame(raw));
  REQUIRE(adts != nullptr);
  CHECK(adts->prefixSize() == 7);
  CHECK(adts->getIndex() == 7);
  CHECK(adts->dts() == 321);
  CHECK(adts->size() == payload.size() + 7);
  CHECK(std::string(adts->data() + 7, adts->size() - 7) == payload);
  const auto* header = reinterpret_cast<const std::uint8_t*>(adts->data());
  CHECK(header[0] == 0xff);
  CHECK((header[1] & 0xf0) == 0xf0);
  const auto frame_length =
      ((header[3] & 0x03) << 11) | (header[4] << 3) | (header[5] >> 5);
  CHECK(frame_length == static_cast<int>(adts->size()));
  auto wrapped_adts = Packet(encoded.stream, adts->toString());
  REQUIRE(track->inputFrame(converter.Convert(wrapped_adts, 342, 342)));
  CHECK(adts->size() == payload.size() + 7);
  packet.Unref();
  CHECK(raw->toString() == payload);
  track->delDelegate(delegate);
}

TEST_CASE("包转换拒绝非法配置且不隐式替换编码格式",
          "[remuxer][shared][packet][validation]") {
  Runtime runtime;
  auto encoded = Video("libx264");
  auto stream = encoded.stream;
  SECTION("未知编码") {
    stream.codec_parameters.get()->codec_id = AV_CODEC_ID_VP9;
  }
  SECTION("重复索引") {
    CHECK_THROWS_AS(PacketConverter({stream, stream}), std::invalid_argument);
    return;
  }
  SECTION("缺少全局配置") {
    av_freep(&stream.codec_parameters.get()->extradata);
    stream.codec_parameters.get()->extradata_size = 0;
  }
  SECTION("截断avcC") { ExtraData(stream, std::string("\1\2\3", 3)); }
  SECTION("无效参数集") {
    ExtraData(stream, std::string("\0\0\0\1\x65\x80", 6));
  }
  CHECK_THROWS_AS(PacketConverter({stream}), std::invalid_argument);
}

TEST_CASE("AAC缺失或不匹配的ASC以及截断hvcC明确失败",
          "[remuxer][shared][packet][configuration]") {
  Runtime runtime;
  SECTION("AAC ASC截断") {
    auto encoded = Audio();
    ExtraData(encoded.stream, std::string("\x11", 1));
    CHECK_THROWS_AS(PacketConverter({encoded.stream}), std::invalid_argument);
  }
  SECTION("AAC采样率不符") {
    auto encoded = Audio();
    encoded.stream.codec_parameters.get()->sample_rate = 44100;
    CHECK_THROWS_AS(PacketConverter({encoded.stream}), std::invalid_argument);
  }
  SECTION("AAC声道数不符") {
    auto encoded = Audio();
    av_channel_layout_uninit(&encoded.stream.codec_parameters.get()->ch_layout);
    encoded.stream.codec_parameters.get()->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    CHECK_THROWS_AS(PacketConverter({encoded.stream}), std::invalid_argument);
  }
  SECTION("hvcC数组截断") {
    auto encoded = Video("libx265");
    PacketConverter converter({encoded.stream});
    auto config = converter.tracks().front()->getExtraData()->toString();
    config.resize(24);
    ExtraData(encoded.stream, config);
    CHECK_THROWS_AS(PacketConverter({encoded.stream}), std::invalid_argument);
  }
}

TEST_CASE("包转换拒绝空包索引时间基及截断NAL且保留零PTS",
          "[remuxer][shared][packet][validation]") {
  Runtime runtime;
  auto encoded = Video("libx264");
  PacketConverter converter({encoded.stream});
  auto invalid = encoded.packets.front().Ref();
  SECTION("空payload") { invalid.Unref(); }
  SECTION("空对象") { auto owner = std::move(invalid); }
  SECTION("未知索引") { invalid->stream_index = 99; }
  SECTION("缺时间基") { invalid->time_base = {0, 1}; }
  SECTION("时间基不符") { invalid->time_base = {1, 100}; }
  SECTION("只有NAL头") {
    invalid = Packet(encoded.stream, std::string("\0\0\0\1\x65", 5));
  }
  SECTION("没有Annex B起始码") {
    invalid = Packet(encoded.stream, std::string("\x65\x80", 2));
  }
  CHECK_THROWS_AS(converter.Convert(invalid, 20, 40), std::invalid_argument);
  auto valid = converter.Convert(encoded.packets.front(), 20, 0);
  CHECK(valid->dts() == 20);
  CHECK(valid->pts() == 0);
  CHECK(valid->cacheAble());
}

}  // namespace

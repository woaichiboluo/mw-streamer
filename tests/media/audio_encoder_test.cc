#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include "mw/decoder/audio_decoder.h"
#include "mw/encoder/audio_encoder.h"
#include "mw/ffmpeg/error.h"

namespace {

using mw::streamer::decoder::AudioDecoder;
using mw::streamer::decoder::AudioDecoderConfig;
using mw::streamer::encoder::AudioEncoder;
using mw::streamer::encoder::AudioEncoderConfig;
using mw::streamer::ffmpeg::Frame;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::ThrowIfError;

constexpr int kSampleRate = 48000;
constexpr int kChannelCount = 1;
constexpr AVRational kTimeBase{1, kSampleRate};

Frame MakeAudioFrame(int sample_count, std::int64_t pts) {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = kSampleRate;
  frame->time_base = kTimeBase;
  frame->nb_samples = sample_count;
  frame->pts = pts;
  frame->duration = sample_count;
  av_channel_layout_default(&frame->ch_layout, kChannelCount);
  ThrowIfError(av_frame_get_buffer(frame.get(), 0), "分配测试音频帧");

  auto* samples = reinterpret_cast<float*>(frame->extended_data[0]);
  for (int index = 0; index < sample_count; ++index) {
    const double time = static_cast<double>(pts + index) / kSampleRate;
    samples[index] = static_cast<float>(
        0.2 * std::sin(2.0 * 3.141592653589793 * 440.0 * time));
  }
  return frame;
}

}  // namespace

TEST_CASE("audio encoder converts FIFO chunks and drains AAC packets") {
  AudioEncoderConfig config;
  config.encoder_name = "aac";
  config.properties = {
      {"b", "96000"},
      {"mw_unknown_audio_option", "ignored"},
  };
  AudioEncoder encoder(config, 3);
  CHECK_FALSE(encoder.is_open());
  CHECK_THROWS_AS(encoder.stream_info(), std::logic_error);

  const std::vector<int> chunk_sizes{480, 960, 240, 1440, 700, 980};
  std::vector<Packet> packets;
  encoder.SetOnPacket(
      [&](const Packet& packet) { packets.push_back(packet.Ref()); });

  std::int64_t pts = 0;
  auto first_frame = MakeAudioFrame(chunk_sizes.front(), pts);
  encoder.Open(first_frame);
  CHECK(encoder.is_open());
  CHECK(encoder.config().encoder_name == "aac");
  CHECK(encoder.stream_info().stream_index == 3);
  CHECK(encoder.stream_info().time_base.num == 1);
  CHECK(encoder.stream_info().time_base.den == kSampleRate);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_type ==
        AVMEDIA_TYPE_AUDIO);
  CHECK(encoder.stream_info().codec_parameters.get()->codec_id ==
        AV_CODEC_ID_AAC);
  CHECK(encoder.stream_info().codec_parameters.get()->sample_rate ==
        kSampleRate);
  CHECK(encoder.stream_info().codec_parameters.get()->extradata_size > 0);
  CHECK(encoder.stream_info().codec_parameters.get()->initial_padding > 0);
  CHECK_THROWS_AS(encoder.Open(first_frame), std::logic_error);

  encoder.Encode(first_frame);
  pts += chunk_sizes.front();
  for (std::size_t index = 1; index < chunk_sizes.size(); ++index) {
    auto frame = MakeAudioFrame(chunk_sizes[index], pts);
    encoder.Encode(frame);
    pts += chunk_sizes[index];
  }
  encoder.Drain();

  REQUIRE_FALSE(packets.empty());
  CHECK(packets.front()->pts < 0);
  std::int64_t previous_dts = packets.front()->dts;
  for (const auto& packet : packets) {
    REQUIRE(packet.get());
    CHECK(packet->stream_index == 3);
    CHECK(packet->size > 0);
    CHECK(packet->dts == packet->pts);
    CHECK(packet->dts >= previous_dts);
    previous_dts = packet->dts;
  }
  CHECK_THROWS_AS(encoder.Encode(first_frame), std::logic_error);
  CHECK_NOTHROW(encoder.Drain());

  AudioDecoderConfig decoder_config;
  decoder_config.decoder_name = "aac";
  AudioDecoder decoder(encoder.stream_info(), decoder_config);
  std::size_t decoded_frames = 0;
  decoder.SetOnFrame([&](const Frame& frame) {
    CHECK(frame->sample_rate == kSampleRate);
    CHECK(frame->ch_layout.nb_channels == kChannelCount);
    CHECK(frame->nb_samples > 0);
    ++decoded_frames;
  });
  for (const auto& packet : packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();
  CHECK(decoded_frames > 0);
}

TEST_CASE("audio encoder rejects frames outside the Processor contract") {
  AudioEncoder encoder;
  auto frame = MakeAudioFrame(480, 0);
  frame->format = AV_SAMPLE_FMT_FLTP;
  CHECK_THROWS_AS(encoder.Open(frame), std::invalid_argument);

  CHECK_THROWS_AS(AudioEncoder(AudioEncoderConfig{}, -1),
                  std::invalid_argument);
}

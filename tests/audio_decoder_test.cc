#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
}

#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/ffmpeg/audio_decoder.h"

namespace {

using mediakit::Frame;
using mediakit::FrameWriterInterface;
using mediakit::MP4Demuxer;
using mediakit::Track;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::ffmpeg::AudioDecoder;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;

std::string SamplePath() {
  return std::string(MW_AUDIO_DECODER_TEST_DATA_DIR) + "/h264_aac.mp4";
}

}  // namespace

TEST_CASE("audio decoder decodes drains and resets an AAC stream") {
  MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath());

  Track::Ptr audio_track;
  for (const auto& track : demuxer.getTracks(false)) {
    if (track->getTrackType() == mediakit::TrackAudio) {
      audio_track = track;
      break;
    }
  }
  REQUIRE(audio_track);

  auto parameters = std::make_shared<ZlmCodecParametersConverter>(audio_track);
  StreamInfo stream_info{1, parameters->codec_parameters(),
                         parameters->time_base()};
  AudioDecoder decoder(stream_info);
  CHECK(decoder.stream_info().stream_index == 1);
  CHECK(decoder.stream_info().codec_parameters.get() !=
        parameters->codec_parameters().get());
  CHECK(decoder.stream_info().codec_parameters.get()->codec_id ==
        parameters->codec_parameters().get()->codec_id);

  std::size_t decoded_frames = 0;
  std::int64_t decoded_samples = 0;
  std::int64_t previous_pts = AV_NOPTS_VALUE;
  bool valid_frames = true;
  decoder.SetOnFrame([&](const mw::streamer::ffmpeg::Frame& decoded_frame) {
    const auto* frame = decoded_frame.get();
    if (!frame || frame->format == AV_SAMPLE_FMT_NONE ||
        frame->sample_rate != 48000 || frame->ch_layout.nb_channels != 1 ||
        frame->nb_samples <= 0 || !frame->extended_data) {
      valid_frames = false;
      return;
    }
    if (frame->pts != AV_NOPTS_VALUE && previous_pts != AV_NOPTS_VALUE &&
        frame->pts < previous_pts) {
      valid_frames = false;
    }
    previous_pts = frame->pts;
    ++decoded_frames;
    decoded_samples += frame->nb_samples;
  });

  auto packet_converter = std::make_shared<ZlmPacketConverter>(audio_track, 1);
  std::vector<Packet> retained_packets;
  packet_converter->SetOnPacket([&](const Packet& packet) {
    retained_packets.push_back(packet.Clone());
    decoder.Decode(packet);
    return true;
  });

  FrameWriterInterface* delegate =
      audio_track->addDelegate([packet_converter](const Frame::Ptr& frame) {
        return packet_converter->InputFrame(frame);
      });
  REQUIRE(delegate);

  bool eof = false;
  while (!eof) {
    bool key_frame = false;
    int error = 0;
    demuxer.readFrame(key_frame, eof, &error);
    REQUIRE(error == 0);
  }
  REQUIRE(packet_converter->Flush());
  decoder.Drain();

  const auto first_pass_frames = decoded_frames;
  const auto first_pass_samples = decoded_samples;
  CHECK(valid_frames);
  CHECK(retained_packets.size() == 95);
  CHECK(first_pass_frames == retained_packets.size());
  CHECK(first_pass_samples ==
        static_cast<std::int64_t>(first_pass_frames) * 1024);
  CHECK_THROWS_AS(decoder.Decode(retained_packets.front()), std::logic_error);
  CHECK_NOTHROW(decoder.Drain());

  decoder.Reset();
  previous_pts = AV_NOPTS_VALUE;
  for (const auto& packet : retained_packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();

  CHECK(valid_frames);
  CHECK(decoded_frames == first_pass_frames * 2);
  CHECK(decoded_samples == first_pass_samples * 2);

  audio_track->delDelegate(delegate);
  demuxer.closeMP4();
}

TEST_CASE("audio decoder rejects invalid stream descriptions") {
  CHECK_THROWS_AS(AudioDecoder(StreamInfo{}), std::invalid_argument);

  CodecParameters parameters;
  parameters.get()->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters.get()->codec_id = AV_CODEC_ID_H264;

  CHECK_THROWS_AS(AudioDecoder(StreamInfo{0, parameters, {1, 1000}}),
                  std::invalid_argument);
}

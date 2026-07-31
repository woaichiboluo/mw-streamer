#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
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
#include "Poller/EventPoller.h"
#include "Record/MP4Demuxer.h"
#include "mw/cache/packet_queue.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/decoder/audio_decoder.h"
#include "mw/resampler/audio_resampler.h"

namespace {

using mediakit::Frame;
using mediakit::FrameWriterInterface;
using mediakit::MP4Demuxer;
using mediakit::Track;
using mw::streamer::cache::PacketQueue;
using mw::streamer::cache::PacketQueueState;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::decoder::AudioDecoder;
using mw::streamer::decoder::AudioDecoderConfig;
using mw::streamer::ffmpeg::CodecParameters;
using mw::streamer::ffmpeg::Packet;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::resampler::AudioResampler;

std::string SamplePath() {
  return std::string(MW_AUDIO_DECODER_TEST_DATA_DIR) + "/h264_aac.mp4";
}

}  // namespace

TEST_CASE("audio decoder decodes drains and flushes an AAC stream") {
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
  AudioDecoderConfig config;
  config.decoder_name = "aac";
  AudioDecoder decoder(stream_info, config);
  AudioResampler resampler(stream_info);
  CHECK(decoder.config().decoder_name == "aac");
  CHECK(decoder.stream_info().stream_index == 1);
  CHECK(decoder.stream_info().codec_parameters.get() !=
        parameters->codec_parameters().get());
  CHECK(decoder.stream_info().codec_parameters.get()->codec_id ==
        parameters->codec_parameters().get()->codec_id);

  AudioDecoderConfig missing_decoder;
  missing_decoder.decoder_name = "mw_missing_audio_decoder";
  CHECK_THROWS_AS(AudioDecoder(stream_info, missing_decoder),
                  std::invalid_argument);

  std::size_t decoded_frames = 0;
  std::int64_t decoded_samples = 0;
  std::int64_t previous_pts = AV_NOPTS_VALUE;
  std::uint64_t decoding_generation = 0;
  std::size_t generation_two_frames = 0;
  std::size_t resampled_frames = 0;
  std::int64_t resampled_samples = 0;
  std::int64_t previous_resampled_pts = AV_NOPTS_VALUE;
  std::size_t generation_two_resampled_frames = 0;
  bool valid_frames = true;
  bool valid_resampled_frames = true;
  resampler.SetOnFrame([&](const mw::streamer::ffmpeg::Frame& output) {
    const auto* frame = output.get();
    if (!frame || frame->format != AV_SAMPLE_FMT_FLT ||
        frame->sample_rate != AudioResampler::kOutputSampleRate ||
        frame->ch_layout.nb_channels != 1 || frame->nb_samples <= 0 ||
        !frame->data[0] || frame->duration != frame->nb_samples ||
        frame->time_base.num != 1 ||
        frame->time_base.den != AudioResampler::kOutputSampleRate ||
        (frame->pts != AV_NOPTS_VALUE &&
         previous_resampled_pts != AV_NOPTS_VALUE &&
         frame->pts < previous_resampled_pts)) {
      valid_resampled_frames = false;
      return;
    }
    previous_resampled_pts = frame->pts;
    ++resampled_frames;
    resampled_samples += frame->nb_samples;
    if (decoding_generation == 2) {
      ++generation_two_resampled_frames;
    }
  });
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
    if (decoding_generation == 2) {
      ++generation_two_frames;
    }
    resampler.Resample(decoded_frame);
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
  resampler.Drain();

  const auto first_pass_frames = decoded_frames;
  const auto first_pass_samples = decoded_samples;
  const auto first_pass_resampled_frames = resampled_frames;
  const auto first_pass_resampled_samples = resampled_samples;
  CHECK(valid_frames);
  CHECK(valid_resampled_frames);
  CHECK(retained_packets.size() == 95);
  CHECK(first_pass_frames == retained_packets.size());
  CHECK(first_pass_samples ==
        static_cast<std::int64_t>(first_pass_frames) * 1024);
  CHECK(first_pass_resampled_frames == first_pass_frames);
  CHECK(first_pass_resampled_samples == first_pass_samples);
  CHECK_THROWS_AS(decoder.Decode(retained_packets.front()), std::logic_error);
  CHECK_NOTHROW(decoder.Drain());

  decoder.Flush();
  resampler.Flush();
  previous_pts = AV_NOPTS_VALUE;
  previous_resampled_pts = AV_NOPTS_VALUE;
  for (const auto& packet : retained_packets) {
    decoder.Decode(packet);
  }
  decoder.Drain();
  resampler.Drain();

  CHECK(valid_frames);
  CHECK(valid_resampled_frames);
  CHECK(decoded_frames == first_pass_frames * 2);
  CHECK(decoded_samples == first_pass_samples * 2);
  CHECK(resampled_frames == first_pass_resampled_frames * 2);
  CHECK(resampled_samples == first_pass_resampled_samples * 2);

  decoder.Flush();
  resampler.Flush();
  previous_pts = AV_NOPTS_VALUE;
  previous_resampled_pts = AV_NOPTS_VALUE;
  auto queue = std::make_shared<PacketQueue>(std::chrono::milliseconds{0});
  std::size_t generation_two_packets = 0;
  std::size_t timeline_resets = 0;
  std::vector<std::uint64_t> ended_generations;
  std::exception_ptr failure;

  queue->poller()->sync([&]() {
    try {
      queue->SetOnPacket([&](std::uint64_t generation, const Packet& packet) {
        decoding_generation = generation;
        decoder.Decode(packet);
        if (generation == 2) {
          ++generation_two_packets;
        }
      });
      queue->SetOnTimelineReset([&](std::uint64_t generation) {
        if (generation != 2) {
          throw std::runtime_error("PacketQueue输出了错误的时间线generation");
        }
        decoder.Flush();
        resampler.Flush();
        previous_pts = AV_NOPTS_VALUE;
        previous_resampled_pts = AV_NOPTS_VALUE;
        ++timeline_resets;
      });
      queue->SetOnGenerationEnd([&](std::uint64_t generation) {
        decoding_generation = generation;
        decoder.Drain();
        resampler.Drain();
        ended_generations.push_back(generation);
      });

      const std::vector<StreamInfo> streams{stream_info};
      queue->SetStreams(1, streams);
      for (std::size_t index = 0; index < retained_packets.size() / 2;
           ++index) {
        if (!queue->Input(1, retained_packets[index])) {
          throw std::runtime_error("PacketQueue拒绝了第一代测试音频包");
        }
      }

      queue->SetStreams(2, streams);
      for (const auto& packet : retained_packets) {
        if (!queue->Input(2, packet)) {
          throw std::runtime_error("PacketQueue拒绝了第二代测试音频包");
        }
      }
      queue->EndInput(2);
    } catch (...) {
      failure = std::current_exception();
    }
  });

  if (failure) {
    std::rethrow_exception(failure);
  }
  CHECK(timeline_resets == 1);
  CHECK(valid_frames);
  CHECK(valid_resampled_frames);
  CHECK(generation_two_packets == retained_packets.size());
  CHECK(generation_two_frames == retained_packets.size());
  CHECK(generation_two_resampled_frames == retained_packets.size());
  CHECK(ended_generations == std::vector<std::uint64_t>{2});
  CHECK(queue->state() == PacketQueueState::kStarved);

  queue->Stop();
  queue.reset();
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

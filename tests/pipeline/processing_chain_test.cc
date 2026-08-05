#include <cstddef>
#include <memory>
#include <optional>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
}

#include <catch2/catch_test_macros.hpp>

#include "Extension/Track.h"
#include "Record/MP4Demuxer.h"
#include "mw/converter/zlm_codec_parameters_converter.h"
#include "mw/converter/zlm_packet_converter.h"
#include "mw/decoder/video_decoder.h"
#include "mw/performance/internal/stage_recorder.h"
#include "mw/pipeline/internal/streaming/audio_processing_chain.h"
#include "mw/pipeline/internal/streaming/video_processing_chain.h"
#include "mw/processor/internal/source_info_adapter.h"
#include "mw/processor/streaming_processor_handler.h"

namespace {

using mediakit::Frame;
using mediakit::FrameWriterInterface;
using mediakit::MP4Demuxer;
using mediakit::Track;
using mw::streamer::converter::ZlmCodecParametersConverter;
using mw::streamer::converter::ZlmPacketConverter;
using mw::streamer::decoder::VideoDecoder;
using mw::streamer::decoder::VideoDecoderBackend;
using mw::streamer::decoder::VideoDecoderConfig;
using mw::streamer::ffmpeg::StreamInfo;
using mw::streamer::performance::internal::TrackRecorder;
using mw::streamer::pipeline::internal::streaming::AudioProcessingChain;
using mw::streamer::pipeline::internal::streaming::VideoProcessingChain;
using mw::streamer::processor::StreamingProcessorHandler;
using mw::streamer::processor::internal::MakeProcessorSourceInfo;

struct CallbackState {
  std::size_t boundaries = 0;
};

std::string SamplePath() {
  return std::string(MW_PROCESSING_CHAIN_TEST_DATA_DIR) + "/h264_aac.mp4";
}

}  // namespace

TEST_CASE("Streaming ProcessingChain串联音视频解码与Processor") {
  MP4Demuxer demuxer;
  demuxer.openMP4(SamplePath());

  Track::Ptr audio_track;
  Track::Ptr video_track;
  for (const auto& track : demuxer.getTracks(false)) {
    if (track->getTrackType() == mediakit::TrackAudio) {
      audio_track = track;
    } else if (track->getTrackType() == mediakit::TrackVideo) {
      video_track = track;
    }
  }
  REQUIRE(audio_track);
  REQUIRE(video_track);

  auto audio_parameters =
      std::make_shared<ZlmCodecParametersConverter>(audio_track);
  auto video_parameters =
      std::make_shared<ZlmCodecParametersConverter>(video_track);
  const std::optional<StreamInfo> audio_stream{StreamInfo{
      1, audio_parameters->codec_parameters(), audio_parameters->time_base()}};
  const std::optional<StreamInfo> video_stream{StreamInfo{
      0, video_parameters->codec_parameters(), video_parameters->time_base()}};

  StreamingProcessorHandler processor(
      MakeProcessorSourceInfo(audio_stream, video_stream), nullptr);
  CallbackState state;
  MwStreamerStreamingProcessorCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_boundary = [](MwStreamerProcessorBoundaryReason reason,
                             void* user_context) {
    CHECK(reason == kMwStreamerProcessorEndOfInput);
    ++static_cast<CallbackState*>(user_context)->boundaries;
  };
  const MwStreamerStreamingProcessorConfig processor_config{64, 64, ""};
  REQUIRE(processor.Start(processor_config, callbacks) ==
          kMwStreamerProcessorStartSuccess);

  std::size_t audio_outputs = 0;
  TrackRecorder audio_performance;
  AudioProcessingChain audio_chain(
      *audio_stream, {}, processor, audio_performance,
      [&](const mw::streamer::ffmpeg::Frame&) { ++audio_outputs; });
  VideoDecoderConfig video_decoder_config;
  video_decoder_config.backend = VideoDecoderBackend::kSoftware;
  std::size_t video_outputs = 0;
  TrackRecorder video_performance;
  VideoProcessingChain video_chain(
      std::make_unique<VideoDecoder>(*video_stream, video_decoder_config),
      processor, video_performance,
      [&](const mw::streamer::ffmpeg::Frame&) { ++video_outputs; });

  auto audio_converter = std::make_shared<ZlmPacketConverter>(audio_track, 1);
  auto video_converter = std::make_shared<ZlmPacketConverter>(video_track, 0);
  audio_converter->SetOnPacket([&](const mw::streamer::ffmpeg::Packet& packet) {
    audio_chain.Input(packet);
    return true;
  });
  video_converter->SetOnPacket([&](const mw::streamer::ffmpeg::Packet& packet) {
    video_chain.Input(packet);
    return true;
  });

  FrameWriterInterface* audio_delegate =
      audio_track->addDelegate([audio_converter](const Frame::Ptr& frame) {
        return audio_converter->InputFrame(frame);
      });
  FrameWriterInterface* video_delegate =
      video_track->addDelegate([video_converter](const Frame::Ptr& frame) {
        return video_converter->InputFrame(frame);
      });
  REQUIRE(audio_delegate);
  REQUIRE(video_delegate);

  bool eof = false;
  while (!eof) {
    bool key_frame = false;
    int error = 0;
    demuxer.readFrame(key_frame, eof, &error);
    REQUIRE(error == 0);
  }
  REQUIRE(audio_converter->Flush());
  REQUIRE(video_converter->Flush());
  audio_chain.Drain();
  video_chain.Drain();
  processor.NotifyBoundary(kMwStreamerProcessorEndOfInput);

  audio_track->delDelegate(audio_delegate);
  video_track->delDelegate(video_delegate);
  demuxer.closeMP4();
  processor.Stop();

  CHECK(state.boundaries == 1);
  CHECK(audio_outputs == 95);
  CHECK(video_outputs == 20);
}

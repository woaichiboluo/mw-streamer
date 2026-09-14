#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <catch2/catch_test_macros.hpp>

#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/sink/custom_sink_node.h"

namespace {

using namespace mw::streamer;

FrameStreamsReady Streams(std::uint64_t generation = 1, int width = 64) {
  StreamInfo video;
  video.stream_index = 0;
  video.time_base = {1, 90000};
  auto* video_parameters = video.codec_parameters.get();
  video_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  video_parameters->codec_id = AV_CODEC_ID_H264;
  video_parameters->width = width;
  video_parameters->height = 32;
  video_parameters->format = AV_PIX_FMT_YUV420P;
  video_parameters->framerate = {25, 1};

  StreamInfo audio;
  audio.stream_index = 1;
  audio.time_base = {1, 44100};
  auto* audio_parameters = audio.codec_parameters.get();
  audio_parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_parameters->codec_id = AV_CODEC_ID_AAC;
  audio_parameters->sample_rate = 44100;
  av_channel_layout_default(&audio_parameters->ch_layout, 2);
  return {generation, {std::move(video), std::move(audio)}, nullptr};
}

Frame Video() {
  Frame frame;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 64;
  frame->height = 32;
  frame->pts = 9000;
  frame->duration = 3600;
  frame->time_base = {1, 90000};
  REQUIRE(av_frame_get_buffer(frame.get(), 32) >= 0);
  return frame;
}

Frame Audio() {
  Frame frame;
  frame->format = AV_SAMPLE_FMT_FLT;
  frame->sample_rate = 48000;
  frame->nb_samples = 4;
  frame->pts = 480;
  frame->duration = 4;
  frame->time_base = {1, 48000};
  av_channel_layout_default(&frame->ch_layout, 2);
  REQUIRE(av_frame_get_buffer(frame.get(), 0) >= 0);
  return frame;
}

struct CallbackState {
  int starts = 0;
  int videos = 0;
  int audios = 0;
  MwStreamerProcessorSourceInfo source{};
  MwStreamerMessageSender sender{};
};

MwStreamerCustomSinkCallbacks Callbacks(CallbackState& state) {
  MwStreamerCustomSinkCallbacks callbacks{};
  callbacks.user_context = &state;
  callbacks.on_start = [](const MwStreamerProcessorSourceInfo* source,
                          const MwStreamerMessageSender* sender,
                          void* context) {
    auto& state = *static_cast<CallbackState*>(context);
    ++state.starts;
    state.source = *source;
    state.sender = *sender;
    return kMwStreamerProcessorStartSuccess;
  };
  callbacks.on_frame = [](const MwStreamerVideoFrameView* frame,
                          void* context) {
    REQUIRE(frame->buffer.width == 64);
    ++static_cast<CallbackState*>(context)->videos;
  };
  callbacks.on_audio = [](const MwStreamerAudioFrameView* frame,
                          void* context) {
    REQUIRE(frame->sample_rate == 48000);
    ++static_cast<CallbackState*>(context)->audios;
  };
  return callbacks;
}

TEST_CASE("Custom Sink exposes source, frames and message sender",
          "[custom-sink]") {
  CallbackState state;
  CustomSink sink("custom", Callbacks(state));
  std::string received_type;
  std::string received_payload;
  sink.SetMessageSender([&](const SinkMessage& message) {
    received_type = message.type;
    received_payload.assign(static_cast<const char*>(message.payload),
                            message.payload_size);
  });

  sink.OnStreamsReady(Streams());
  sink.OnVideoFrame({1, Video()});
  sink.OnAudioFrame({1, Audio()});

  REQUIRE(state.starts == 1);
  CHECK(state.source.has_video == 1);
  CHECK(state.source.has_audio == 1);
  CHECK(state.source.video.width == 64);
  CHECK(state.source.audio.sample_rate == 44100);
  CHECK(state.videos == 1);
  CHECK(state.audios == 1);

  const char payload[] = "control";
  state.sender.send(state.sender.context, "seek", payload,
                    std::strlen(payload), nullptr);
  CHECK(received_type == "seek");
  CHECK(received_payload == "control");
  sink.Stop();
}

TEST_CASE("Custom Sink starts once across stable generations",
          "[custom-sink]") {
  CallbackState state;
  CustomSink sink("custom", Callbacks(state));
  sink.OnStreamsReady(Streams(1));
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect});
  sink.OnStreamsReady(Streams(2));
  CHECK(state.starts == 1);
  sink.OnVideoFrame({2, Video()});
  CHECK(state.videos == 1);
  sink.Stop();
}

TEST_CASE("Custom Sink rejects changed source information",
          "[custom-sink]") {
  CallbackState state;
  CustomSink sink("custom", Callbacks(state));
  sink.OnStreamsReady(Streams(1));
  sink.OnTimelineReset({2, TimelineResetReason::kReconnect});
  CHECK_THROWS_AS(sink.OnStreamsReady(Streams(2, 128)),
                  std::invalid_argument);
  CHECK(state.starts == 1);
}

}  // namespace

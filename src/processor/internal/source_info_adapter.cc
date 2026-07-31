#include "mw/processor/internal/source_info_adapter.h"

#include <cstdint>

#include "mw/media/internal/codec_bridge.h"

namespace mw::streamer::processor::internal {
namespace {

MwStreamerRational ToProcessorRational(AVRational value) {
  return {value.num, value.den};
}

}  // namespace

MwStreamerProcessorSourceInfo MakeProcessorSourceInfo(
    const std::optional<ffmpeg::StreamInfo>& audio_stream,
    const std::optional<ffmpeg::StreamInfo>& video_stream) {
  MwStreamerProcessorSourceInfo source_info{};
  if (video_stream) {
    const auto* parameters = video_stream->codec_parameters.get();
    source_info.has_video = 1;
    source_info.video.codec =
        media::internal::ToMwStreamerCodec(parameters->codec_id);
    source_info.video.width = static_cast<std::uint32_t>(parameters->width);
    source_info.video.height = static_cast<std::uint32_t>(parameters->height);
    source_info.video.frame_rate =
        parameters->framerate.num > 0 && parameters->framerate.den > 0
            ? ToProcessorRational(parameters->framerate)
            : MwStreamerRational{0, 1};
    source_info.video.time_base = ToProcessorRational(video_stream->time_base);
  }
  if (audio_stream) {
    const auto* parameters = audio_stream->codec_parameters.get();
    source_info.has_audio = 1;
    source_info.audio.codec =
        media::internal::ToMwStreamerCodec(parameters->codec_id);
    source_info.audio.sample_rate =
        static_cast<std::uint32_t>(parameters->sample_rate);
    source_info.audio.channel_count =
        static_cast<std::uint32_t>(parameters->ch_layout.nb_channels);
    source_info.audio.time_base = ToProcessorRational(audio_stream->time_base);
  }
  return source_info;
}

}  // namespace mw::streamer::processor::internal

#include "mw/streamer/internal/processor_context.h"

#include <cstddef>
#include <vector>

extern "C" {
#include <libavcodec/codec_par.h>
}

#include "mw/streamer/internal/audio_converter.h"
#include "mw/streamer/internal/common/error.h"
#include "mw/streamer/internal/common/timestamp.h"
#include "mw/streamer/internal/video_color.h"

namespace mw::streamer::internal {

MwInputSourceInfo MakeInputSourceInfo(std::size_t input_index,
                                      const StreamInfo& video_stream,
                                      const StreamInfo* audio_stream) {
  const AVCodecParameters* video = video_stream.codec_parameters.get();
  if (video_stream.index < 0 || video == nullptr || video->width <= 0 ||
      video->height <= 0) {
    ThrowError(StatusCode::kInvalidArgument,
               "Processor input requires valid video stream information");
  }

  MwInputSourceInfo result{};
  result.input_index = input_index;
  result.video.present = 1;
  result.video.width = video->width;
  result.video.height = video->height;
  result.video.frame_rate =
      IsValidTimeBase(video_stream.frame_rate)
          ? MwRational{video_stream.frame_rate.num, video_stream.frame_rate.den}
          : MwRational{0, 1};
  result.video.color_range = ToMwColorRange(video->color_range);
  result.video.color_space = ToMwColorSpace(video->color_space);

  if (audio_stream != nullptr) {
    const AVCodecParameters* audio = audio_stream->codec_parameters.get();
    if (audio_stream->index < 0 || audio == nullptr ||
        audio->sample_rate <= 0 || audio->ch_layout.nb_channels <= 0) {
      ThrowError(StatusCode::kInvalidArgument,
                 "Processor input contains invalid audio stream information");
    }
    result.audio.present = 1;
    result.audio.sample_rate = audio->sample_rate;
    result.audio.channel_count = audio->ch_layout.nb_channels;
  }
  return result;
}

MwProcessorContext MakeProcessorContext(
    int device_id, const std::vector<MwInputSourceInfo>& input_sources,
    std::size_t primary_video_input_index,
    const VideoEncoderConfig& video_encoder, bool has_audio) {
  if (device_id < 0 || input_sources.empty() ||
      primary_video_input_index >= input_sources.size()) {
    ThrowError(StatusCode::kInvalidArgument,
               "Processor context configuration is invalid");
  }

  MwProcessorContext result{};
  result.device_id = device_id;
  result.inputs = input_sources.data();
  result.input_count = input_sources.size();
  result.video_output.width = video_encoder.width;
  result.video_output.height = video_encoder.height;
  result.video_output.frame_rate = video_encoder.frame_rate;
  const MwVideoSourceInfo& primary_video =
      input_sources[primary_video_input_index].video;
  result.video_output.color_range =
      primary_video.color_range == MW_VIDEO_COLOR_RANGE_UNSPECIFIED
          ? MW_VIDEO_COLOR_RANGE_LIMITED
          : primary_video.color_range;
  result.video_output.color_space = primary_video.color_space;
  if (has_audio) {
    result.audio_output.sample_rate = kProcessorAudioSampleRate;
    result.audio_output.channel_count = kProcessorAudioChannelCount;
    result.audio_output.block_frame_count = kProcessorAudioBlockFrameCount;
  }
  return result;
}

}  // namespace mw::streamer::internal

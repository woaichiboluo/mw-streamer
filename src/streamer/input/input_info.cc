#include "mw/streamer/input/internal/input_info.h"

#include <utility>

#include "mw/streamer/ffmpeg/av_dictionary.h"

namespace mw::streamer::internal {

AvInputInfo MakeInputInfo(AVFormatContext& format) {
  AvInputInfo info;
  info.timestamp_discontinuities =
      (format.iformat->flags & AVFMT_TS_DISCONT) != 0;
  info.start_time_us = format.start_time;
  info.duration_us = format.duration;
  info.metadata = ffmpeg::AvDictionary::ToMap(format.metadata);
  for (unsigned int index = 0; index < format.nb_streams; ++index) {
    auto* stream = format.streams[index];
    ffmpeg::StreamInfo description{stream->index,
                                   ffmpeg::CodecParameters(*stream->codecpar),
                                   stream->time_base};
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      description.frame_rate = av_guess_frame_rate(&format, stream, nullptr);
      description.sample_aspect_ratio =
          av_guess_sample_aspect_ratio(&format, stream, nullptr);
    }
    description.disposition = stream->disposition;
    description.metadata = ffmpeg::AvDictionary::ToMap(stream->metadata);
    info.streams.push_back(std::move(description));
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        info.video_stream_index < 0) {
      info.video_stream_index = stream->index;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
               info.audio_stream_index < 0) {
      info.audio_stream_index = stream->index;
    }
  }
  return info;
}

}  // namespace mw::streamer::internal

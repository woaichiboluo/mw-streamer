#include "mw/streamer/converter/zlm_codec_parameters_converter.h"

#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace mw::streamer::internal {

constexpr std::array kCodecMappings{
    std::pair{AV_CODEC_ID_H264, mediakit::CodecH264},
    std::pair{AV_CODEC_ID_HEVC, mediakit::CodecH265},
    std::pair{AV_CODEC_ID_AAC, mediakit::CodecAAC},
    std::pair{AV_CODEC_ID_PCM_ALAW, mediakit::CodecG711A},
    std::pair{AV_CODEC_ID_PCM_MULAW, mediakit::CodecG711U},
    std::pair{AV_CODEC_ID_OPUS, mediakit::CodecOpus},
    std::pair{AV_CODEC_ID_MJPEG, mediakit::CodecJPEG},
    std::pair{AV_CODEC_ID_VP8, mediakit::CodecVP8},
    std::pair{AV_CODEC_ID_VP9, mediakit::CodecVP9},
};

mediakit::CodecId ToZlmCodecId(AVCodecID codec_id) noexcept {
  for (const auto& [ffmpeg_codec, zlm_codec] : kCodecMappings) {
    if (ffmpeg_codec == codec_id) {
      return zlm_codec;
    }
  }
  return mediakit::CodecInvalid;
}

AVCodecID ToFfmpegCodecId(mediakit::CodecId codec_id) noexcept {
  for (const auto& [ffmpeg_codec, zlm_codec] : kCodecMappings) {
    if (zlm_codec == codec_id) {
      return ffmpeg_codec;
    }
  }
  return AV_CODEC_ID_NONE;
}

static std::vector<std::uint8_t> GetExtraData(const mediakit::Track::Ptr& track) {
  std::vector<std::uint8_t> result;
  const auto codec = track->getCodecId();
  if (codec == mediakit::CodecH264 || codec == mediakit::CodecH265) {
    auto video = std::static_pointer_cast<mediakit::VideoTrack>(track);
    for (const auto& frame : video->getConfigFrames()) {
      if (frame && frame->data() && frame->size()) {
        result.insert(result.end(), frame->data(),
                      frame->data() + frame->size());
      }
    }
    return result;
  }

  if (!track->ready()) {
    return result;
  }
  auto extra = track->getExtraData();
  if (extra && extra->data() && extra->size()) {
    result.assign(extra->data(), extra->data() + extra->size());
  }
  return result;
}

static void SetExtraData(AVCodecParameters* parameters,
                  const std::vector<std::uint8_t>& extra_data) {
  if (extra_data.empty()) {
    return;
  }
  if (extra_data.size() > INT_MAX) {
    throw std::length_error("codec extradata过大");
  }

  parameters->extradata = static_cast<std::uint8_t*>(
      av_mallocz(extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!parameters->extradata) {
    throw std::bad_alloc();
  }
  std::memcpy(parameters->extradata, extra_data.data(), extra_data.size());
  parameters->extradata_size = static_cast<int>(extra_data.size());
}

ZlmCodecParametersConverter::ZlmCodecParametersConverter(
    const mediakit::Track::Ptr& track) {
  if (!track) {
    throw std::invalid_argument("track不能为空");
  }

  const auto codec_id = ToFfmpegCodecId(track->getCodecId());
  if (codec_id == AV_CODEC_ID_NONE) {
    throw std::invalid_argument("不支持的ZLM codec");
  }

  auto* parameters = codec_parameters_.get();
  parameters->codec_id = codec_id;
  parameters->bit_rate = track->getBitRate();

  if (track->getTrackType() == mediakit::TrackVideo) {
    auto video = std::static_pointer_cast<mediakit::VideoTrack>(track);
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->width = video->getVideoWidth();
    parameters->height = video->getVideoHeight();
    if (video->getVideoFps() > 0) {
      parameters->framerate = av_d2q(video->getVideoFps(), 100000);
    }
  } else if (track->getTrackType() == mediakit::TrackAudio) {
    auto audio = std::static_pointer_cast<mediakit::AudioTrack>(track);
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->sample_rate = audio->getAudioSampleRate();
    parameters->bits_per_raw_sample = audio->getAudioSampleBit();
    av_channel_layout_default(&parameters->ch_layout, audio->getAudioChannel());
  } else {
    throw std::invalid_argument("不支持的ZLM track类型");
  }

  SetExtraData(parameters, GetExtraData(track));
}

const ffmpeg::CodecParameters&
ZlmCodecParametersConverter::codec_parameters() const {
  return codec_parameters_;
}

AVRational ZlmCodecParametersConverter::time_base() const {
  return kZlmTimeBase;
}

}  // namespace mw::streamer::internal

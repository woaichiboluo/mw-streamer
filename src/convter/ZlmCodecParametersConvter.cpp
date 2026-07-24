#include "mw/convter/ZlmCodecParametersConvter.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

namespace mw::convter {
namespace {

constexpr AVRational kZlmTimeBase{1, 1000};

AVCodecID getFFmpegCodecId(mediakit::CodecId codec) {
  switch (codec) {
    case mediakit::CodecH264:
      return AV_CODEC_ID_H264;
    case mediakit::CodecH265:
      return AV_CODEC_ID_HEVC;
    case mediakit::CodecAAC:
      return AV_CODEC_ID_AAC;
    case mediakit::CodecG711A:
      return AV_CODEC_ID_PCM_ALAW;
    case mediakit::CodecG711U:
      return AV_CODEC_ID_PCM_MULAW;
    case mediakit::CodecOpus:
      return AV_CODEC_ID_OPUS;
    case mediakit::CodecJPEG:
      return AV_CODEC_ID_MJPEG;
    case mediakit::CodecVP8:
      return AV_CODEC_ID_VP8;
    case mediakit::CodecVP9:
      return AV_CODEC_ID_VP9;
    default:
      return AV_CODEC_ID_NONE;
  }
}

std::vector<std::uint8_t> getExtraData(
    const mediakit::Track::Ptr& track) {
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

void setExtraData(AVCodecParameters* parameters,
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

}  // namespace

ZlmCodecParametersConvter::ZlmCodecParametersConvter(
    const mediakit::Track::Ptr& track) {
  if (!track) {
    throw std::invalid_argument("track不能为空");
  }

  const auto codec_id = getFFmpegCodecId(track->getCodecId());
  if (codec_id == AV_CODEC_ID_NONE) {
    throw std::invalid_argument("不支持的ZLM codec");
  }

  codec_parameters_.reset(
      avcodec_parameters_alloc(), [](AVCodecParameters* parameters) {
        avcodec_parameters_free(&parameters);
      });
  if (!codec_parameters_) {
    throw std::bad_alloc();
  }

  codec_parameters_->codec_id = codec_id;
  codec_parameters_->bit_rate = track->getBitRate();

  if (track->getTrackType() == mediakit::TrackVideo) {
    auto video = std::static_pointer_cast<mediakit::VideoTrack>(track);
    codec_parameters_->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_parameters_->width = video->getVideoWidth();
    codec_parameters_->height = video->getVideoHeight();
    if (video->getVideoFps() > 0) {
      codec_parameters_->framerate = av_d2q(video->getVideoFps(), 100000);
    }
  } else if (track->getTrackType() == mediakit::TrackAudio) {
    auto audio = std::static_pointer_cast<mediakit::AudioTrack>(track);
    codec_parameters_->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_parameters_->sample_rate = audio->getAudioSampleRate();
    codec_parameters_->bits_per_raw_sample = audio->getAudioSampleBit();
    av_channel_layout_default(&codec_parameters_->ch_layout,
                              audio->getAudioChannel());
  } else {
    throw std::invalid_argument("不支持的ZLM track类型");
  }

  setExtraData(codec_parameters_.get(), getExtraData(track));
}

const ZlmCodecParametersConvter::CodecParametersPtr&
ZlmCodecParametersConvter::getCodecParameters() const {
  return codec_parameters_;
}

AVRational ZlmCodecParametersConvter::getTimeBase() const {
  return kZlmTimeBase;
}

}  // namespace mw::convter

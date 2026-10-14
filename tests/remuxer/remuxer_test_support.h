#ifndef MW_STREAMER_TESTS_REMUXER_REMUXER_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_REMUXER_REMUXER_TEST_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mw/streamer/ffmpeg/error.h"
#include "mw/streamer/ffmpeg/frame.h"
#include "mw/streamer/ffmpeg/stream_info.h"

namespace remuxer_test {

namespace ffmpeg = mw::streamer::ffmpeg;
constexpr AVRational kVideoTimeBase{1, 50};
constexpr AVRational kAudioTimeBase{1, 48000};
constexpr std::int64_t kVideoStart = 500;
constexpr std::int64_t kAudioStart = 480000;

inline ffmpeg::StreamInfo VideoStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 4;
  stream.time_base = kVideoTimeBase;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AV_CODEC_ID_H264;
  parameters->format = AV_PIX_FMT_YUV420P;
  parameters->width = 64;
  parameters->height = 64;
  parameters->sample_aspect_ratio = {1, 1};
  return stream;
}

inline ffmpeg::StreamInfo AudioStream() {
  ffmpeg::StreamInfo stream;
  stream.stream_index = 7;
  stream.time_base = kAudioTimeBase;
  auto* parameters = stream.codec_parameters.get();
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AV_CODEC_ID_AAC;
  parameters->format = AV_SAMPLE_FMT_FLTP;
  parameters->sample_rate = 48000;
  parameters->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  return stream;
}

inline ffmpeg::Frame VideoFrame(int index) {
  ffmpeg::Frame frame;
  frame->width = 64;
  frame->height = 64;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->time_base = kVideoTimeBase;
  frame->pts = kVideoStart + index;
  frame->duration = 1;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配视频测试帧");
  for (int plane = 0; plane < 3; ++plane) {
    const int size = plane == 0 ? 64 : 32;
    const int value = plane == 0 ? 20 + index * 20 : 128;
    for (int row = 0; row < size; ++row) {
      std::memset(frame->data[plane] + row * frame->linesize[plane], value,
                  static_cast<std::size_t>(size));
    }
  }
  return frame;
}

inline ffmpeg::Frame AudioFrame(int samples, std::int64_t start = kAudioStart,
                                int leading_samples = 0) {
  ffmpeg::Frame frame;
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->sample_rate = 48000;
  frame->nb_samples = samples;
  frame->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  frame->time_base = kAudioTimeBase;
  frame->pts = start;
  frame->duration = samples;
  ffmpeg::FfmpegException::throwIfError(av_frame_get_buffer(frame.get(), 0),
                                        "分配音频测试帧");
  for (int channel = 0; channel < 2; ++channel) {
    auto* data = reinterpret_cast<float*>(frame->extended_data[channel]);
    for (int sample = 0; sample < samples; ++sample) {
      data[sample] = sample < leading_samples ? 0.75F : 0.1F;
    }
  }
  return frame;
}

}  // namespace remuxer_test

#endif  // MW_STREAMER_TESTS_REMUXER_REMUXER_TEST_SUPPORT_H_

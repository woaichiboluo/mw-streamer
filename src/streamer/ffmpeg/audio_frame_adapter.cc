#include "mw/streamer/ffmpeg/audio_frame_adapter.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer::ffmpeg::internal {
namespace {

void Check(int result, const char* operation) {
  FfmpegException::throwIfError(result, operation);
}

}  // namespace

AudioFrameAdapter::AudioFrameAdapter(AVSampleFormat input,
                                     AVSampleFormat output, int sample_rate,
                                     const AVChannelLayout& layout,
                                     int frame_size)
    : time_base_{1, sample_rate} {
  if (sample_rate <= 0 || frame_size < 0 ||
      av_get_bytes_per_sample(input) <= 0 ||
      av_get_bytes_per_sample(output) <= 0 ||
      !av_channel_layout_check(&layout)) {
    throw std::invalid_argument("音频适配配置无效");
  }
  source_ = avfilter_graph_alloc_filter(graph_.get(),
                                        avfilter_get_by_name("abuffer"), "in");
  sink_ = avfilter_graph_alloc_filter(
      graph_.get(), avfilter_get_by_name("abuffersink"), "out");
  if (!source_ || !sink_) throw std::bad_alloc();
  auto* parameters = av_buffersrc_parameters_alloc();
  if (!parameters) throw std::bad_alloc();
  const std::unique_ptr<AVBufferSrcParameters, void (*)(AVBufferSrcParameters*)>
      guard(parameters, [](AVBufferSrcParameters* value) {
        av_channel_layout_uninit(&value->ch_layout);
        av_free(value);
      });
  parameters->format = input;
  parameters->sample_rate = sample_rate;
  parameters->time_base = time_base_;
  Check(av_channel_layout_copy(&parameters->ch_layout, &layout),
        "设置音频适配源布局");
  Check(av_buffersrc_parameters_set(source_, parameters), "设置音频适配源参数");
  Check(avfilter_init_str(source_, nullptr), "初始化音频适配源");
  const AVSampleFormat formats[]{output};
  const int rates[]{sample_rate};
  Check(av_opt_set_array(sink_, "sample_formats",
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0, 1,
                         AV_OPT_TYPE_SAMPLE_FMT, formats),
        "设置音频适配输出格式");
  Check(av_opt_set_array(sink_, "samplerates",
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0, 1,
                         AV_OPT_TYPE_INT, rates),
        "设置音频适配输出采样率");
  Check(avfilter_init_str(sink_, nullptr), "初始化音频适配输出");
  Check(avfilter_link(source_, 0, sink_, 0), "连接音频适配滤镜");
  Check(avfilter_graph_config(graph_.get(), nullptr), "配置音频适配滤镜");
  if (frame_size > 0) {
    av_buffersink_set_frame_size(sink_, static_cast<unsigned int>(frame_size));
  }
}

void AudioFrameAdapter::Submit(const Frame& frame) {
  if (frame->pts == AV_NOPTS_VALUE || frame->time_base.num <= 0 ||
      frame->time_base.den <= 0 || frame->duration < 0) {
    throw std::invalid_argument("音频适配输入需要有效的 PTS、时间基和非负时长");
  }
  // abuffer interprets PTS in its configured input time base. Reblocking
  // retains the caller's timeline; only the timestamp unit changes here.
  auto submitted = frame.Ref();
  submitted->pts = av_rescale_q(frame->pts, frame->time_base, time_base_);
  submitted->duration =
      av_rescale_q(frame->duration, frame->time_base, time_base_);
  submitted->time_base = time_base_;
  if (submitted->pts == AV_NOPTS_VALUE ||
      submitted->pts == std::numeric_limits<std::int64_t>::max() ||
      submitted->duration < 0 ||
      submitted->duration == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("音频适配输入时间基换算溢出");
  }
  Check(av_buffersrc_write_frame(source_, submitted.get()), "提交音频适配源帧");
}

void AudioFrameAdapter::End() {
  Check(av_buffersrc_close(source_, AV_NOPTS_VALUE, 0), "结束音频适配源");
}

bool AudioFrameAdapter::Receive(Frame& frame) {
  const int result = av_buffersink_get_frame(sink_, frame.get());
  if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return false;
  Check(result, "获取音频适配帧");
  frame->time_base = av_buffersink_get_time_base(sink_);
  return true;
}

}  // namespace mw::streamer::ffmpeg::internal

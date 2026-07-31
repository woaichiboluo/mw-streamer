#include "mw/processor/internal/audio_frame_allocator.h"

#include <stdexcept>

#include "mw/ffmpeg/error.h"

namespace mw::streamer::processor::internal {

AudioFrameAllocator::~AudioFrameAllocator() {
  av_channel_layout_uninit(&channel_layout_);
}

ffmpeg::Frame AudioFrameAllocator::Allocate(const ffmpeg::Frame& input) {
  if (!input.get()) {
    throw std::invalid_argument("不能根据空音频Frame分配输出");
  }
  PrepareOrValidate(*input.get());

  ffmpeg::Frame output;
  output->format = input->format;
  output->sample_rate = input->sample_rate;
  output->nb_samples = input->nb_samples;
  ffmpeg::ThrowIfError(
      av_channel_layout_copy(&output->ch_layout, &channel_layout_),
      "复制Processor输出音频声道布局");
  ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 0),
                       "分配Processor音频输出帧");
  return output;
}

void AudioFrameAllocator::PrepareOrValidate(const AVFrame& input) {
  if (!prepared_) {
    AVChannelLayout pending_layout{};
    ffmpeg::ThrowIfError(
        av_channel_layout_copy(&pending_layout, &input.ch_layout),
        "初始化Processor音频分配器");
    channel_layout_ = pending_layout;
    prepared_ = true;
    return;
  }
  if (av_channel_layout_compare(&channel_layout_, &input.ch_layout) != 0) {
    throw std::invalid_argument("当前链路不支持动态改变音频声道布局");
  }
}

}  // namespace mw::streamer::processor::internal

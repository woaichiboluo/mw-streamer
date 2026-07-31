#include "mw/resampler/audio_resampler.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "mw/ffmpeg/error.h"
#include "mw/log/logging.h"

namespace mw::streamer::resampler {
namespace {

using Log = log::Module<log::LogModule::kStreamer>;

constexpr AVRational kOutputTimeBase{1, AudioResampler::kOutputSampleRate};

void ValidateStreamInfo(const ffmpeg::StreamInfo& stream_info) {
  stream_info.Validate();
  const auto* parameters = stream_info.codec_parameters.get();
  if (parameters->codec_type != AVMEDIA_TYPE_AUDIO) {
    throw std::invalid_argument("AudioResampler只接受音频流");
  }
  if (parameters->sample_rate <= 0 ||
      av_channel_layout_check(&parameters->ch_layout) != 1) {
    throw std::invalid_argument("音频流缺少有效的采样率或声道布局");
  }
}

}  // namespace

class AudioResampler::Impl final {
 public:
  explicit Impl(ffmpeg::StreamInfo stream_info)
      : stream_info_(std::move(stream_info)) {
    ValidateStreamInfo(stream_info_);
  }

  ~Impl() { swr_free(&context_); }

  void SetOnFrame(OnFrame callback) { on_frame_ = std::move(callback); }

  void Resample(const ffmpeg::Frame& frame) {
    if (drained_) {
      throw std::logic_error(
          "AudioResampler已Drain，必须Flush后才能继续重采样");
    }

    const auto* input = frame.get();
    ValidateFrame(input);
    EnsureInitialized(*input);

    const auto output_pts = OutputPts(*input);
    const int output_capacity =
        swr_get_out_samples(context_, input->nb_samples);
    ffmpeg::ThrowIfError(output_capacity, "计算音频重采样输出容量");
    auto output = AllocateOutputFrame(std::max(output_capacity, 1));
    const int output_samples =
        swr_convert(context_, output->extended_data, output->nb_samples,
                    const_cast<const std::uint8_t**>(input->extended_data),
                    input->nb_samples);
    ffmpeg::ThrowIfError(output_samples, "重采样音频帧");
    Emit(std::move(output), output_samples, output_pts);
  }

  void Drain() {
    if (drained_) {
      return;
    }

    while (context_) {
      const int output_capacity = swr_get_out_samples(context_, 0);
      ffmpeg::ThrowIfError(output_capacity, "计算音频重采样尾部容量");
      if (output_capacity == 0) {
        break;
      }

      auto output = AllocateOutputFrame(output_capacity);
      const int output_samples = swr_convert(context_, output->extended_data,
                                             output->nb_samples, nullptr, 0);
      ffmpeg::ThrowIfError(output_samples, "排空音频重采样器");
      if (output_samples == 0) {
        break;
      }
      Emit(std::move(output), output_samples,
           next_output_pts_.value_or(AV_NOPTS_VALUE));
    }
    drained_ = true;
    Log::Debug("音频重采样器已排空: stream_index={}",
               stream_info_.stream_index);
  }

  void Flush() {
    if (context_) {
      swr_close(context_);
      ffmpeg::ThrowIfError(swr_init(context_), "重置音频重采样器");
    }
    next_output_pts_.reset();
    drained_ = false;
    Log::Debug("音频重采样器已刷新: stream_index={}",
               stream_info_.stream_index);
  }

  const ffmpeg::StreamInfo& stream_info() const noexcept {
    return stream_info_;
  }

 private:
  void ValidateFrame(const AVFrame* frame) const {
    const auto* parameters = stream_info_.codec_parameters.get();
    if (!frame || frame->nb_samples <= 0 || !frame->extended_data ||
        frame->sample_rate != parameters->sample_rate ||
        av_channel_layout_check(&frame->ch_layout) != 1 ||
        av_channel_layout_compare(&frame->ch_layout, &parameters->ch_layout) !=
            0 ||
        frame->format == AV_SAMPLE_FMT_NONE ||
        av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format)) <=
            0) {
      throw std::invalid_argument("音频帧为空或采样率、声道布局、采样格式无效");
    }
    if (context_ && frame->format != input_sample_format_) {
      throw std::invalid_argument("当前链路不支持动态改变音频采样格式");
    }
  }

  void EnsureInitialized(const AVFrame& input) {
    if (context_) {
      return;
    }

    SwrContext* pending_context = nullptr;
    const auto* parameters = stream_info_.codec_parameters.get();
    try {
      ffmpeg::ThrowIfError(
          swr_alloc_set_opts2(&pending_context, &parameters->ch_layout,
                              AV_SAMPLE_FMT_FLT, kOutputSampleRate,
                              &input.ch_layout,
                              static_cast<AVSampleFormat>(input.format),
                              input.sample_rate, 0, nullptr),
          "配置音频重采样器");
      ffmpeg::ThrowIfError(swr_init(pending_context), "初始化音频重采样器");
    } catch (...) {
      swr_free(&pending_context);
      throw;
    }

    context_ = pending_context;
    input_sample_format_ = static_cast<AVSampleFormat>(input.format);
    const char* input_format_name =
        av_get_sample_fmt_name(input_sample_format_);
    Log::Info(
        "音频重采样器已初始化: stream_index={}, input_format={}, "
        "input_sample_rate={}, output_format=flt, output_sample_rate={}, "
        "channels={}",
        stream_info_.stream_index,
        input_format_name ? input_format_name : "unknown", input.sample_rate,
        kOutputSampleRate, input.ch_layout.nb_channels);
  }

  ffmpeg::Frame AllocateOutputFrame(int sample_capacity) const {
    ffmpeg::Frame output;
    output->format = AV_SAMPLE_FMT_FLT;
    output->sample_rate = kOutputSampleRate;
    output->time_base = kOutputTimeBase;
    output->nb_samples = sample_capacity;
    ffmpeg::ThrowIfError(
        av_channel_layout_copy(&output->ch_layout,
                               &stream_info_.codec_parameters.get()->ch_layout),
        "复制重采样输出声道布局");
    ffmpeg::ThrowIfError(av_frame_get_buffer(output.get(), 0),
                         "分配重采样音频帧");
    return output;
  }

  std::int64_t OutputPts(const AVFrame& input) {
    if (input.pts == AV_NOPTS_VALUE) {
      return next_output_pts_.value_or(AV_NOPTS_VALUE);
    }

    const auto input_pts =
        av_rescale_q(input.pts, stream_info_.time_base, kOutputTimeBase);
    const auto delay = swr_get_delay(context_, kOutputSampleRate);
    return input_pts - delay;
  }

  void Emit(ffmpeg::Frame output, int output_samples, std::int64_t output_pts) {
    if (output_samples == 0) {
      if (output_pts != AV_NOPTS_VALUE) {
        next_output_pts_ = output_pts;
      }
      return;
    }

    output->nb_samples = output_samples;
    output->pts = output_pts;
    output->duration = output_samples;
    if (output_pts == AV_NOPTS_VALUE) {
      next_output_pts_.reset();
    } else {
      next_output_pts_ = output_pts + output_samples;
    }
    if (on_frame_) {
      on_frame_(output);
    }
  }

  ffmpeg::StreamInfo stream_info_;
  SwrContext* context_ = nullptr;
  AVSampleFormat input_sample_format_ = AV_SAMPLE_FMT_NONE;
  std::optional<std::int64_t> next_output_pts_;
  OnFrame on_frame_;
  bool drained_ = false;
};

AudioResampler::AudioResampler(ffmpeg::StreamInfo stream_info)
    : impl_(std::make_unique<Impl>(std::move(stream_info))) {}

AudioResampler::~AudioResampler() = default;

void AudioResampler::SetOnFrame(OnFrame callback) {
  impl_->SetOnFrame(std::move(callback));
}

void AudioResampler::Resample(const ffmpeg::Frame& frame) {
  impl_->Resample(frame);
}

void AudioResampler::Drain() { impl_->Drain(); }

void AudioResampler::Flush() { impl_->Flush(); }

const ffmpeg::StreamInfo& AudioResampler::stream_info() const noexcept {
  return impl_->stream_info();
}

}  // namespace mw::streamer::resampler

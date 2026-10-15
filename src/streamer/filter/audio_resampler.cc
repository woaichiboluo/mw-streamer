#include "mw/streamer/filter/audio_resampler.h"

#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

extern "C" {
#include <libswresample/swresample.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

using ffmpeg::FfmpegException;

bool ValidTimeBase(AVRational value) { return value.num > 0 && value.den > 0; }

std::int64_t Rescale(std::int64_t value, AVRational from, AVRational to) {
  const auto result = av_rescale_q(value, from, to);
  if (result == AV_NOPTS_VALUE ||
      result == std::numeric_limits<std::int64_t>::max()) {
    throw std::overflow_error("AudioResampler 时间戳超出可表示范围");
  }
  return result;
}

std::int64_t Multiply(std::int64_t value, int multiplier) {
  if (value > std::numeric_limits<std::int64_t>::max() / multiplier ||
      value <= std::numeric_limits<std::int64_t>::min() / multiplier) {
    throw std::overflow_error("AudioResampler 时间戳乘积超出可表示范围");
  }
  return value * multiplier;
}

std::int64_t AddSamples(std::int64_t pts, int samples) {
  if (pts > std::numeric_limits<std::int64_t>::max() - samples) {
    throw std::overflow_error("AudioResampler 输出末尾时间超出可表示范围");
  }
  return pts + samples;
}

struct ChannelLayout {
  AVChannelLayout value{};
  ~ChannelLayout() { av_channel_layout_uninit(&value); }
};

}  // namespace

class AudioResampler::Impl final {
 public:
  explicit Impl(AudioResamplerConfig config)
      : rate_(config.sample_rate),
        format_(config.sample_format),
        capacity_(config.input_capacity) {
    if (rate_ <= 0 || av_get_bytes_per_sample(format_) <= 0 ||
        !av_channel_layout_check(&config.channel_layout) || capacity_ == 0) {
      throw std::invalid_argument("AudioResampler 目标音频格式和容量无效");
    }
    FfmpegException::throwIfError(
        av_channel_layout_copy(&layout_.value, &config.channel_layout),
        "复制重采样目标声道布局");
    MW_LOG_INFO("streamer",
                "AudioResampler 配置: instance={} rate={} format={} "
                "channels={} capacity={}",
                fmt::ptr(this), rate_, av_get_sample_fmt_name(format_),
                layout_.value.nb_channels, capacity_);
  }

  ~Impl() { swr_free(&context_); }

  bool TryPushFrame(const AvDecoderReadResult& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || failure_) return false;
    if (inputs_.size() >= capacity_) {
      MW_LOG_TRACE("streamer", "AudioResampler 队列满拒绝提交: queue={}/{}",
                   inputs_.size(), capacity_);
      return false;
    }
    Validate(input);
    inputs_.push_back(input);
    return true;
  }

  AvDecoderReadResult TryReadFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    if (stopped_) return {};
    try {
      while (!inputs_.empty()) {
        const auto& input = inputs_.front();
        const bool eof = input.status == AvDecoderReadStatus::kEof;
        if (context_ && (eof || !Matches(input))) {
          auto output = Drain();
          if (output.status == AvDecoderReadStatus::kFrame) return output;
          ClearConverter();
        }
        if (eof) {
          auto output = input;
          output.time_base = time_base();
          if (output.end_pts != AV_NOPTS_VALUE) {
            output.end_pts =
                Rescale(output.end_pts, input.time_base, time_base());
          }
          MW_LOG_TRACE("streamer",
                       "AudioResampler 输出 EOF: stream={} "
                       "end_pts={} time_base=1/{}",
                       output.stream_index, output.end_pts, rate_);
          PopInput();
          return output;
        }
        if (!context_) Initialize(input);
        auto output = Convert(input);
        PopInput();
        if (output.status == AvDecoderReadStatus::kFrame) return output;
      }
      return {};
    } catch (...) {
      failure_ = std::current_exception();
      throw;
    }
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    MW_LOG_INFO("streamer", "AudioResampler 停止清理: queued={}",
                inputs_.size());
    stopped_ = true;
    inputs_.clear();
    ClearConverter();
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    MW_LOG_INFO("streamer", "AudioResampler 重置: queued={}", inputs_.size());
    inputs_.clear();
    ClearConverter();
    failure_ = nullptr;
    stopped_ = false;
  }

  AVRational time_base() const { return AVRational{1, rate_}; }

 private:
  void Validate(const AvDecoderReadResult& input) const {
    if (input.status == AvDecoderReadStatus::kEof) {
      if (input.frame || (input.end_pts != AV_NOPTS_VALUE &&
                          !ValidTimeBase(input.time_base))) {
        throw std::invalid_argument("AudioResampler EOF 参数无效");
      }
      if (input.end_pts != AV_NOPTS_VALUE) {
        Rescale(input.end_pts, input.time_base, time_base());
      }
      return;
    }
    if (input.status != AvDecoderReadStatus::kFrame || !input.frame ||
        !input.frame->get()) {
      throw std::invalid_argument("AudioResampler 需要音频帧或 EOF");
    }
    const auto* frame = input.frame->get();
    const auto format = static_cast<AVSampleFormat>(frame->format);
    if (frame->hw_frames_ctx || frame->width != 0 || frame->height != 0 ||
        frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
        av_get_bytes_per_sample(format) <= 0 ||
        !av_channel_layout_check(&frame->ch_layout) ||
        frame->pts == AV_NOPTS_VALUE || !ValidTimeBase(input.time_base) ||
        !frame->extended_data) {
      throw std::invalid_argument("AudioResampler 需要有效 CPU 音频和时间戳");
    }
    const int planes =
        av_sample_fmt_is_planar(format) ? frame->ch_layout.nb_channels : 1;
    for (int plane = 0; plane < planes; ++plane) {
      if (!frame->extended_data[plane]) {
        throw std::invalid_argument("AudioResampler 音频平面为空");
      }
    }
    const auto source_pts =
        Rescale(frame->pts, input.time_base, AVRational{1, frame->sample_rate});
    Multiply(source_pts, rate_);
    Rescale(frame->pts, input.time_base, time_base());
  }

  bool Matches(const AvDecoderReadResult& input) const {
    const auto* previous = last_input_->frame->get();
    const auto* current = input.frame->get();
    return input.stream_index == last_input_->stream_index &&
           current->sample_rate == previous->sample_rate &&
           current->format == previous->format &&
           av_channel_layout_compare(&current->ch_layout,
                                     &previous->ch_layout) == 0;
  }

  void Initialize(const AvDecoderReadResult& input) {
    const auto* frame = input.frame->get();
    FfmpegException::throwIfError(
        swr_alloc_set_opts2(&context_, &layout_.value, format_, rate_,
                            &frame->ch_layout,
                            static_cast<AVSampleFormat>(frame->format),
                            frame->sample_rate, 0, nullptr),
        "创建音频重采样器");
    FfmpegException::throwIfError(swr_init(context_), "初始化音频重采样器");
    MW_LOG_INFO(
        "streamer",
        "AudioResampler 建立格式: stream={} "
        "input_rate={} input_format={} input_channels={} "
        "output_rate={} output_format={} output_channels={}",
        input.stream_index, frame->sample_rate,
        av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)),
        frame->ch_layout.nb_channels, rate_, av_get_sample_fmt_name(format_),
        layout_.value.nb_channels);
  }

  ffmpeg::Frame Allocate(int capacity, const ffmpeg::Frame& source) const {
    ffmpeg::Frame output;
    output.CopyPropertiesFrom(source);
    auto* frame = output.get();
    frame->sample_rate = rate_;
    frame->format = format_;
    frame->nb_samples = capacity;
    av_channel_layout_uninit(&frame->ch_layout);
    FfmpegException::throwIfError(
        av_channel_layout_copy(&frame->ch_layout, &layout_.value),
        "复制重采样输出声道布局");
    FfmpegException::throwIfError(av_frame_get_buffer(frame, 0),
                                  "分配重采样输出缓冲");
    return output;
  }

  AvDecoderReadResult Output(ffmpeg::Frame frame, int samples,
                             std::int64_t pts) {
    if (samples == 0) return {};
    frame->nb_samples = samples;
    frame->pts = pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->best_effort_timestamp = pts;
    frame->time_base = time_base();
    frame->duration = samples;
    next_output_pts_ = AddSamples(pts, samples);
    MW_LOG_TRACE("streamer",
                 "AudioResampler 输出帧: stream={} pts={} "
                 "samples={} time_base=1/{}",
                 last_input_->stream_index, pts, samples, rate_);
    AvDecoderReadResult result;
    result.status = AvDecoderReadStatus::kFrame;
    result.frame = std::move(frame);
    result.stream_index = last_input_->stream_index;
    result.time_base = time_base();
    return result;
  }

  AvDecoderReadResult Convert(const AvDecoderReadResult& input) {
    const auto* source = input.frame->get();
    const auto pts = Multiply(Rescale(source->pts, input.time_base,
                                      AVRational{1, source->sample_rate}),
                              rate_);
    const auto output_pts = swr_next_pts(context_, pts);
    if (output_pts == AV_NOPTS_VALUE) {
      throw std::overflow_error("AudioResampler 无法计算输出时间戳");
    }
    const auto target_pts = Rescale(
        output_pts, AVRational{1, source->sample_rate}, AVRational{1, 1});
    const int capacity = swr_get_out_samples(context_, source->nb_samples);
    FfmpegException::throwIfError(capacity, "计算重采样输出容量");
    auto frame = Allocate(capacity > 0 ? capacity : 1, *input.frame);
    MW_LOG_TRACE("streamer",
                 "AudioResampler 转换输入: stream={} pts={} "
                 "time_base={}/{} samples={} output_pts={}",
                 input.stream_index, source->pts, input.time_base.num,
                 input.time_base.den, source->nb_samples, target_pts);
    const int samples =
        swr_convert(context_, frame->extended_data, frame->nb_samples,
                    const_cast<const std::uint8_t**>(source->extended_data),
                    source->nb_samples);
    FfmpegException::throwIfError(samples, "转换音频采样");
    last_input_ = input;
    next_output_pts_ = target_pts;
    return Output(std::move(frame), samples, target_pts);
  }

  AvDecoderReadResult Drain() {
    const int capacity = swr_get_out_samples(context_, 0);
    FfmpegException::throwIfError(capacity, "计算重采样排空容量");
    auto frame = Allocate(capacity > 0 ? capacity : 1, *last_input_->frame);
    const int samples = swr_convert(context_, frame->extended_data,
                                    frame->nb_samples, nullptr, 0);
    FfmpegException::throwIfError(samples, "排空音频重采样缓存");
    MW_LOG_TRACE("streamer", "AudioResampler 排空延迟: stream={} samples={}",
                 last_input_->stream_index, samples);
    return Output(std::move(frame), samples, *next_output_pts_);
  }

  void PopInput() { inputs_.pop_front(); }

  void ClearConverter() {
    swr_free(&context_);
    last_input_.reset();
    next_output_pts_.reset();
  }

  const int rate_;
  const AVSampleFormat format_;
  const std::size_t capacity_;
  ChannelLayout layout_;
  SwrContext* context_ = nullptr;
  std::mutex mutex_;
  std::deque<AvDecoderReadResult> inputs_;
  std::optional<AvDecoderReadResult> last_input_;
  std::optional<std::int64_t> next_output_pts_;
  bool stopped_ = false;
  std::exception_ptr failure_;
};

AudioResampler::AudioResampler(AudioResamplerConfig config)
    : impl_(std::make_unique<Impl>(config)) {}
AudioResampler::~AudioResampler() = default;
bool AudioResampler::TryPushFrame(const AvDecoderReadResult& input) {
  return impl_->TryPushFrame(input);
}
AvDecoderReadResult AudioResampler::TryReadFrame() {
  return impl_->TryReadFrame();
}
void AudioResampler::Stop() { impl_->Stop(); }
void AudioResampler::Reset() { impl_->Reset(); }
AVRational AudioResampler::time_base() const { return impl_->time_base(); }

}  // namespace mw::streamer

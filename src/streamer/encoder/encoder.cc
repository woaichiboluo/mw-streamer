#include "mw/streamer/encoder/encoder.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include "mw/log.h"
#include "mw/streamer/ffmpeg/error.h"

namespace mw::streamer {
namespace {

constexpr std::size_t kFrameCapacity = 1;

class Options final {
 public:
  ~Options() { av_dict_free(&value_); }
  void Set(const char* name, const std::string& value) {
    ffmpeg::FfmpegException::throwIfError(
        av_dict_set(&value_, name, value.c_str(), 0), "设置编码选项");
  }
  AVDictionary* get() const noexcept { return value_; }

 private:
  AVDictionary* value_ = nullptr;
};

bool ReservedOption(std::string_view key) {
  constexpr std::string_view reserved[] = {
      "b",           "bit_rate",     "g",
      "gop_size",    "bf",           "max_b_frames",
      "maxrate",     "rc_max_rate",  "minrate",
      "rc_min_rate", "bufsize",      "rc_buffer_size",
      "rc",          "cbr",          "framerate",
      "r",           "time_base",    "video_size",
      "s",           "width",        "height",
      "pix_fmt",     "pixel_format", "sample_fmt",
      "sample_rate", "ar",           "channel_layout",
      "ac"};
  return std::find(std::begin(reserved), std::end(reserved), key) !=
         std::end(reserved);
}

void CopyOptions(const std::map<std::string, std::string>& source,
                 Options& target) {
  for (const auto& [key, value] : source) {
    if (ReservedOption(key)) {
      throw std::invalid_argument("扩展选项不能覆盖正式编码字段: " + key);
    }
    target.Set(key.c_str(), value);
  }
}

bool IsNvenc(std::string_view name) {
  return name.size() >= 6 && name.substr(name.size() - 6) == "_nvenc";
}

void ConfigureRateControl(const EncoderConfig& config, Options& options) {
  const auto& name = config.video_encoder_name;
  const bool cbr = config.rate_control == RateControl::kCbr;
  if (config.rate_control != RateControl::kCbr &&
      config.rate_control != RateControl::kVbr) {
    throw std::invalid_argument("未知码率控制模式");
  }
  if (!IsNvenc(name) && name != "libx264" && name != "libx265") {
    throw std::invalid_argument(
        "当前码率控制支持 libx264、libx265 和 NVENC 编码器");
  }
  if (config.video_bit_rate <= 0 || config.max_bit_rate < 0 ||
      (config.max_bit_rate > 0 &&
       (config.max_bit_rate < config.video_bit_rate ||
        (cbr && config.max_bit_rate != config.video_bit_rate)))) {
    throw std::invalid_argument("视频目标码率与峰值码率不兼容");
  }
  const auto maximum = cbr ? config.video_bit_rate : config.max_bit_rate;
  options.Set("b", std::to_string(config.video_bit_rate));
  options.Set("maxrate", std::to_string(maximum));
  if (cbr) options.Set("minrate", std::to_string(config.video_bit_rate));
  if (maximum > 0) {
    if (maximum > std::numeric_limits<int>::max() / 2) {
      throw std::invalid_argument("两秒码率缓冲超过 FFmpeg 支持范围");
    }
    options.Set("bufsize", std::to_string(maximum * 2));
  }
  if (IsNvenc(name)) {
    options.Set("rc", cbr ? "cbr" : "vbr");
  } else if (cbr && name == "libx264") {
    options.Set("nal-hrd", "cbr");
  } else if (cbr) {
    std::string parameters;
    const auto supplied = config.video_options.find("x265-params");
    if (supplied != config.video_options.end() && !supplied->second.empty()) {
      parameters = supplied->second + ":";
    }
    options.Set("x265-params", parameters + "strict-cbr=1:hrd=1");
  }
}

class AudioFilter final {
 public:
  AudioFilter(AVSampleFormat input, AVSampleFormat output, int rate,
              const AVChannelLayout& layout, int frame_size) {
    graph_.reset(avfilter_graph_alloc());
    if (!graph_) throw std::bad_alloc();
    source_ = avfilter_graph_alloc_filter(
        graph_.get(), avfilter_get_by_name("abuffer"), "in");
    sink_ = avfilter_graph_alloc_filter(
        graph_.get(), avfilter_get_by_name("abuffersink"), "out");
    if (!source_ || !sink_) throw std::bad_alloc();
    auto* parameters = av_buffersrc_parameters_alloc();
    if (!parameters) throw std::bad_alloc();
    const std::unique_ptr<AVBufferSrcParameters,
                          void (*)(AVBufferSrcParameters*)>
        guard(parameters, [](AVBufferSrcParameters* value) {
          av_channel_layout_uninit(&value->ch_layout);
          av_free(value);
        });
    parameters->format = input;
    parameters->sample_rate = rate;
    parameters->time_base = {1, rate};
    Check(av_channel_layout_copy(&parameters->ch_layout, &layout),
          "设置编码音频源布局");
    Check(av_buffersrc_parameters_set(source_, parameters),
          "设置编码音频源参数");
    Check(avfilter_init_str(source_, nullptr), "初始化编码音频源");
    const AVSampleFormat formats[]{output};
    const int rates[]{rate};
    Check(av_opt_set_array(sink_, "sample_formats",
                           AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0, 1,
                           AV_OPT_TYPE_SAMPLE_FMT, formats),
          "设置编码音频输出格式");
    Check(av_opt_set_array(sink_, "samplerates",
                           AV_OPT_SEARCH_CHILDREN | AV_OPT_ARRAY_REPLACE, 0, 1,
                           AV_OPT_TYPE_INT, rates),
          "设置编码音频输出采样率");
    Check(avfilter_init_str(sink_, nullptr), "初始化编码音频输出");
    Check(avfilter_link(source_, 0, sink_, 0), "连接编码音频滤镜");
    Check(avfilter_graph_config(graph_.get(), nullptr), "配置编码音频滤镜");
    if (frame_size > 0) {
      av_buffersink_set_frame_size(sink_,
                                   static_cast<unsigned int>(frame_size));
    }
  }

  void Submit(const ffmpeg::Frame& frame) {
    Check(av_buffersrc_write_frame(source_, frame.get()), "提交编码音频源帧");
  }
  void End() {
    Check(av_buffersrc_close(source_, AV_NOPTS_VALUE, 0), "结束编码音频源");
  }
  bool Receive(ffmpeg::Frame& frame) {
    const int result = av_buffersink_get_frame(sink_, frame.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return false;
    Check(result, "获取编码音频滤镜帧");
    frame->time_base = av_buffersink_get_time_base(sink_);
    return true;
  }

 private:
  static void Check(int result, const char* operation) {
    ffmpeg::FfmpegException::throwIfError(result, operation);
  }
  struct DeleteGraph {
    void operator()(AVFilterGraph* graph) const noexcept {
      avfilter_graph_free(&graph);
    }
  };
  std::unique_ptr<AVFilterGraph, DeleteGraph> graph_;
  AVFilterContext* source_ = nullptr;
  AVFilterContext* sink_ = nullptr;
};

}  // namespace

Encoder::~Encoder() { Stop(); }

void Encoder::SetOnReady(OnReady callback) noexcept {
  on_ready_ = std::move(callback);
}
void Encoder::SetOnPacket(OnPacket callback) noexcept {
  on_packet_ = std::move(callback);
}
void Encoder::SetOnEnded(OnEnded callback) noexcept {
  on_ended_ = std::move(callback);
}
void Encoder::SetOnError(OnError callback) noexcept {
  on_error_ = std::move(callback);
}

void Encoder::Start(const EncoderConfig& config,
                    const std::vector<ffmpeg::StreamInfo>& streams,
                    const ffmpeg::HwDeviceContext& video_device) {
  {
    std::lock_guard lock(mutex_);
    if (started_) throw std::logic_error("Encoder 已经启动");
  }
  const ffmpeg::StreamInfo* video = nullptr;
  const ffmpeg::StreamInfo* audio = nullptr;
  for (const auto& stream : streams) {
    stream.Validate();
    const auto type = stream.codec_parameters.get()->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO) {
      if (video) throw std::invalid_argument("Encoder 只支持一条视频轨道");
      video = &stream;
    } else if (type == AVMEDIA_TYPE_AUDIO) {
      if (audio) throw std::invalid_argument("Encoder 只支持一条音频轨道");
      audio = &stream;
    }
  }
  if (video && audio && video->stream_index == audio->stream_index) {
    throw std::invalid_argument("音视频轨道索引不能相同");
  }
  try {
    config_ = config;
    std::vector<ffmpeg::StreamInfo> output;
    if (video) {
      if (config.fps.num <= 0 || config.fps.den <= 0 || config.width < 0 ||
          config.height < 0 || ((config.width == 0) != (config.height == 0))) {
        throw std::invalid_argument("视频尺寸或帧率配置无效");
      }
      const auto* parameters = video->codec_parameters.get();
      ffmpeg::VideoEncoderConfig native;
      native.encoder_name = config.video_encoder_name;
      const auto* codec =
          avcodec_find_encoder_by_name(native.encoder_name.c_str());
      if (codec && (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)) {
        native.thread_type = FF_THREAD_SLICE;
      }
      native.width = config.width ? config.width : parameters->width;
      native.height = config.height ? config.height : parameters->height;
      native.pixel_format = config.video_pixel_format == AV_PIX_FMT_NONE
                                ? static_cast<AVPixelFormat>(parameters->format)
                                : config.video_pixel_format;
      native.frame_rate = config.fps;
      native.time_base = av_inv_q(config.fps);
      native.bit_rate = config.video_bit_rate;
      native.gop_size = config.gop_size;
      native.max_b_frames = config.max_b_frames;
      native.sample_aspect_ratio = parameters->sample_aspect_ratio;
      if (native.sample_aspect_ratio.den <= 0) {
        native.sample_aspect_ratio = {1, 1};
      }
      native.color_range = parameters->color_range;
      native.color_space = parameters->color_space;
      native.color_primaries = parameters->color_primaries;
      native.color_trc = parameters->color_trc;
      Options options;
      CopyOptions(config.video_options, options);
      ConfigureRateControl(config, options);
      if (IsNvenc(native.encoder_name)) {
        video_encoder_ = std::make_unique<ffmpeg::VideoEncoder>(
            native, video_device, options.get());
      } else {
        video_encoder_ =
            std::make_unique<ffmpeg::VideoEncoder>(native, options.get());
      }
      video_stream_index_ = video->stream_index;
      output.push_back(video_encoder_->stream_info(video_stream_index_));
    }
    if (audio) {
      const auto* parameters = audio->codec_parameters.get();
      if (config.audio_bit_rate <= 0) {
        throw std::invalid_argument("音频码率必须大于零");
      }
      ffmpeg::AudioEncoderConfig native;
      native.encoder_name = config.audio_encoder_name;
      audio_input_format_ = static_cast<AVSampleFormat>(parameters->format);
      native.sample_format = audio_input_format_;
      const auto* codec =
          avcodec_find_encoder_by_name(native.encoder_name.c_str());
      if (!codec)
        throw ffmpeg::FfmpegException(AVERROR_ENCODER_NOT_FOUND,
                                      "查找音频编码器");
      if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
        native.thread_type = FF_THREAD_SLICE;
      }
      const void* supported = nullptr;
      int supported_count = 0;
      ffmpeg::FfmpegException::throwIfError(
          avcodec_get_supported_config(nullptr, codec,
                                       AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                       &supported, &supported_count),
          "获取音频编码器采样格式");
      if (supported && supported_count > 0) {
        const auto* formats = static_cast<const AVSampleFormat*>(supported);
        if (std::find(formats, formats + supported_count,
                      native.sample_format) == formats + supported_count) {
          native.sample_format = formats[0];
        }
      }
      native.sample_rate = parameters->sample_rate;
      native.channel_layout = parameters->ch_layout;
      native.bit_rate = config.audio_bit_rate;
      Options options;
      CopyOptions(config.audio_options, options);
      audio_encoder_ =
          std::make_unique<ffmpeg::AudioEncoder>(native, options.get());
      audio_stream_index_ = audio->stream_index;
      audio_format_ = native.sample_format;
      audio_rate_ = native.sample_rate;
      ffmpeg::FfmpegException::throwIfError(
          av_channel_layout_copy(&audio_layout_, &native.channel_layout),
          "复制音频编码声道布局");
      audio_capabilities_ =
          avcodec_find_encoder_by_name(native.encoder_name.c_str())
              ->capabilities;
      output.push_back(audio_encoder_->stream_info(audio_stream_index_));
    }
    {
      std::lock_guard lock(mutex_);
      started_ = true;
      stopping_ = false;
      draining_ = false;
      failed_ = false;
      video_done_ = false;
      audio_done_ = !audio_encoder_;
      ended_notified_ = false;
      performance_.Start(this, static_cast<bool>(video_encoder_),
                         static_cast<bool>(audio_encoder_));
    }
    if (on_ready_) on_ready_(output);
    if (audio_encoder_) audio_thread_ = std::thread(&Encoder::RunAudio, this);
    video_thread_ = std::thread(&Encoder::RunVideo, this);
    MW_LOG_INFO("streamer", "Encoder started: video={} audio={}",
                static_cast<bool>(video_encoder_),
                static_cast<bool>(audio_encoder_));
  } catch (...) {
    RequestCancel();
    Stop();
    throw;
  }
}

bool Encoder::SubmitFrame(const MediaFrame& frame) {
  std::lock_guard lock(mutex_);
  if (!started_ || stopping_ || draining_ || failed_) {
    return false;
  }
  if (static_cast<bool>(video_encoder_) != frame.video.has_value() ||
      static_cast<bool>(audio_encoder_) != frame.audio.has_value()) {
    throw std::invalid_argument("编码输入 frame 与已配置音视频轨道不匹配");
  }
  if (frame.video && (!frame.video->get() || !(*frame.video)->data[0])) {
    throw std::invalid_argument("编码输入视频内容为空");
  }
  const auto samples =
      frame.audio && frame.audio->get() ? (*frame.audio)->nb_samples : 0;
  if (frame.audio &&
      (!frame.audio->get() || !(*frame.audio)->data[0] ||
       (*frame.audio)->format != audio_input_format_ ||
       (*frame.audio)->sample_rate != audio_rate_ || samples <= 0 ||
       av_channel_layout_compare(&(*frame.audio)->ch_layout, &audio_layout_) !=
           0)) {
    throw std::invalid_argument("音频输入格式与编码配置不匹配");
  }
  if ((frame.video && video_pts_ == std::numeric_limits<std::int64_t>::max()) ||
      audio_pts_ > std::numeric_limits<std::int64_t>::max() - samples)
    throw std::overflow_error("编码 frame 时间计数溢出");
  MediaFrame retained = frame;
  if (retained.video) {
    (*retained.video)->pts = video_pts_++;
    (*retained.video)->duration = 1;
    (*retained.video)->time_base = av_inv_q(config_.fps);
  }
  if (retained.audio) {
    (*retained.audio)->pts = audio_pts_;
    (*retained.audio)->duration = samples;
    (*retained.audio)->time_base = {1, audio_rate_};
    audio_pts_ += samples;
  }
  const bool dropped = frames_.size() == kFrameCapacity;
  if (!dropped) frames_.push_back(std::move(retained));
  performance_.AcceptFrame(static_cast<bool>(video_encoder_), samples,
                           frames_.size(), dropped);
  if (dropped) MW_LOG_TRACE("streamer", "Encoder 实时缓存满，丢弃输入 frame");
  wake_.notify_all();
  return true;
}

void Encoder::Drain() noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || failed_) return;
    draining_ = true;
    if (video_done_ && audio_done_ && !ended_notified_) {
      ended_notified_ = true;
      notify = true;
      ++callback_calls_;
    }
  }
  wake_.notify_all();
  if (notify) {
    if (on_ended_) on_ended_();
    {
      std::lock_guard lock(mutex_);
      --callback_calls_;
    }
    wake_.notify_all();
  }
}

void Encoder::RequestCancel() noexcept {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
}

void Encoder::Stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    if (started_ && !failed_ && !stopping_)
      draining_ = true;
    else
      stopping_ = true;
  }
  wake_.notify_all();
  if (video_thread_.joinable()) video_thread_.join();
  if (audio_thread_.joinable()) audio_thread_.join();
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return callback_calls_ == 0; });
  if (stopping_ || failed_) {
    performance_.Abandon(true);
    performance_.Abandon(false);
  }
  performance_.Finish();
  Clear();
}

void Encoder::Clear() noexcept {
  frames_.clear();
  active_frame_.reset();
  video_encoder_.reset();
  audio_encoder_.reset();
  av_channel_layout_uninit(&audio_layout_);
  video_pts_ = audio_pts_ = 0;
  audio_pending_ = audio_finish_requested_ = false;
  audio_completed_ = true;
  video_stream_index_ = -1;
  audio_stream_index_ = -1;
  audio_input_format_ = AV_SAMPLE_FMT_NONE;
  started_ = false;
  video_done_ = true;
  audio_done_ = true;
}

bool Encoder::Cancelled() noexcept {
  std::lock_guard lock(mutex_);
  return stopping_ || failed_;
}

ffmpeg::EncodeResult Encoder::ReceivePackets(ffmpeg::Encoder& encoder,
                                             bool video) {
  ffmpeg::Packet packet;
  for (;;) {
    if (Cancelled()) return ffmpeg::EncodeResult::kEnd;
    const auto result = encoder.ReceivePacket(packet);
    if (result != ffmpeg::EncodeResult::kPacket) return result;
    {
      std::lock_guard lock(mutex_);
      performance_.Packet(video, packet->size);
    }
    packet->stream_index = video ? video_stream_index_ : audio_stream_index_;
    if (!Cancelled() && on_packet_) on_packet_(packet);
  }
}

void Encoder::EncodeFrame(ffmpeg::Encoder& encoder, const ffmpeg::Frame& frame,
                          bool video) {
  while (!Cancelled() && !encoder.SendFrame(frame)) {
    ReceivePackets(encoder, video);
  }
  if (!Cancelled()) ReceivePackets(encoder, video);
}

void Encoder::DrainCodec(ffmpeg::Encoder& encoder, bool video) {
  while (!Cancelled() && !encoder.Drain()) ReceivePackets(encoder, video);
  if (!Cancelled() &&
      ReceivePackets(encoder, video) != ffmpeg::EncodeResult::kEnd) {
    throw std::runtime_error("编码器排空后仍要求输入");
  }
}

void Encoder::RunVideo() noexcept {
  try {
    for (;;) {
      std::optional<ffmpeg::Frame> video;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || failed_ || draining_ || !frames_.empty();
        });
        if (stopping_ || failed_) break;
        if (frames_.empty()) {
          if (draining_) break;
          continue;
        }
        active_frame_ = std::move(frames_.front());
        frames_.pop_front();
        if (active_frame_->video) video = *active_frame_->video;
        audio_pending_ = static_cast<bool>(audio_encoder_);
        audio_completed_ = !audio_pending_;
        performance_.AudioDequeued(frames_.size());
      }
      wake_.notify_all();
      if (video) {
        internal::EncoderPerformance::TimePoint started;
        {
          std::lock_guard lock(mutex_);
          started = performance_.Begin(true);
        }
        auto prepared = video_encoder_->PrepareFrame(*video);
        EncodeFrame(*video_encoder_, prepared, true);
        {
          std::lock_guard lock(mutex_);
          if (!stopping_ && !failed_)
            performance_.Encoded(true, started, 0, frames_.size());
        }
      }
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock,
                   [this] { return stopping_ || failed_ || audio_completed_; });
        if (stopping_ || failed_) break;
        active_frame_.reset();
      }
      wake_.notify_all();
    }
    if (!Cancelled()) {
      {
        std::lock_guard lock(mutex_);
        audio_finish_requested_ = true;
      }
      wake_.notify_all();
      if (video_encoder_) DrainCodec(*video_encoder_, true);
    }
  } catch (const ffmpeg::FfmpegException& error) {
    Fail(error.error_code(), error.what(), true);
  } catch (const std::exception& error) {
    Fail(AVERROR_EXTERNAL, error.what(), true);
  } catch (...) {
    Fail(AVERROR_EXTERNAL, "未知视频编码错误", true);
  }
  FinishTrack(true);
}

void Encoder::RunAudio() noexcept {
  try {
    AudioFilter filter(audio_input_format_, audio_format_, audio_rate_,
                       audio_layout_,
                       audio_capabilities_ & AV_CODEC_CAP_VARIABLE_FRAME_SIZE
                           ? 0
                           : audio_encoder_->frame_size());
    const auto receive = [&] {
      while (!Cancelled()) {
        ffmpeg::Frame frame;
        if (!filter.Receive(frame)) break;
        internal::EncoderPerformance::TimePoint started;
        {
          std::lock_guard lock(mutex_);
          started = performance_.Begin(false);
        }
        EncodeFrame(*audio_encoder_, frame, false);
        {
          std::lock_guard lock(mutex_);
          if (!stopping_ && !failed_)
            performance_.Encoded(false, started, frame->nb_samples,
                                 frames_.size());
        }
      }
    };
    for (;;) {
      std::optional<ffmpeg::Frame> audio;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || failed_ || audio_pending_ ||
                 audio_finish_requested_;
        });
        if (stopping_ || failed_) break;
        if (!audio_pending_) {
          if (audio_finish_requested_) break;
          continue;
        }
        audio = active_frame_->audio;
        audio_pending_ = false;
      }
      filter.Submit(*audio);
      receive();
      {
        std::lock_guard lock(mutex_);
        audio_completed_ = true;
      }
      wake_.notify_all();
    }
    if (!Cancelled()) {
      filter.End();
      receive();
      DrainCodec(*audio_encoder_, false);
    }
  } catch (const ffmpeg::FfmpegException& error) {
    Fail(error.error_code(), error.what(), false);
  } catch (const std::exception& error) {
    Fail(AVERROR_EXTERNAL, error.what(), false);
  } catch (...) {
    Fail(AVERROR_EXTERNAL, "未知音频编码错误", false);
  }
  FinishTrack(false);
}

void Encoder::FinishTrack(bool video) noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    (video ? video_done_ : audio_done_) = true;
    if (video_done_ && audio_done_) {
      if (stopping_ || failed_) {
        performance_.Abandon(true);
        performance_.Abandon(false);
      }
      performance_.Finish();
    }
    if (draining_ && !stopping_ && !failed_ && video_done_ && audio_done_ &&
        !ended_notified_) {
      ended_notified_ = true;
      notify = true;
    }
  }
  wake_.notify_all();
  if (notify && on_ended_) on_ended_();
}

void Encoder::Fail(int error, std::string_view message, bool video) noexcept {
  bool notify = false;
  {
    std::lock_guard lock(mutex_);
    if (!stopping_ && !failed_) {
      failed_ = true;
      performance_.Error(video);
      notify = true;
    }
  }
  wake_.notify_all();
  if (notify) {
    MW_LOG_ERROR("streamer", "Encoder failed: code={} error={}", error,
                 message);
    if (on_error_) on_error_(error, message);
  }
}

}  // namespace mw::streamer
